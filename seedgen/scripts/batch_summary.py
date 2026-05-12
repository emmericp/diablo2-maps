#!/usr/bin/env python3
"""Batched parallel tell-summary driver.

Reads a seed-filter file (packed uint32 LE seeds, sorted ascending), slices
it into fixed-size chunks (default 1M seeds per chunk), and spawns up to
--jobs `mapdump.exe summary` workers in parallel. Each worker:

  - gets its own chunk sub-filter file
  - is invoked with a bounded [start, end] seed range (the min/max of the
    chunk) so the inner iteration only walks the relevant slice
  - writes its own tells.csv into a per-chunk directory

Per-chunk subprocesses bound the LoadAct memory leak (CLAUDE.md). The
caller can stitch the per-chunk CSVs together afterward (see
scripts/csvs_to_tellsbin.py).

Resume support: any chunk whose CSV already exists with the expected
row count is skipped. Ctrl+C stops submitting new chunks and sends
CTRL_BREAK_EVENT to live children; a second Ctrl+C escalates to kill.
"""

import argparse
import signal
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    from tqdm.auto import tqdm
except ImportError:
    sys.exit("ERROR: tqdm not installed. Run: pip install tqdm")

SCRIPT_DIR  = Path(__file__).resolve().parent
DEFAULT_EXE = SCRIPT_DIR.parent / "build" / "Release" / "mapdump.exe"

STOP_REQUESTED = False
POPEN_LIST     = []
POPEN_LOCK     = threading.Lock()


def terminate_all_children(force=False):
    with POPEN_LOCK:
        children = list(POPEN_LIST)
    for proc in children:
        try:
            if force:
                proc.kill()
            elif sys.platform == "win32":
                proc.send_signal(signal.CTRL_BREAK_EVENT)
            else:
                proc.terminate()
        except (OSError, ValueError):
            pass


def signal_handler(sig, frame):
    global STOP_REQUESTED
    if STOP_REQUESTED:
        print("\n[!] Second Ctrl+C — forcing kill on running children.",
              file=sys.stderr)
        terminate_all_children(force=True)
        return
    print("\n[!] Ctrl+C — stopping workers (Ctrl+C again to force-kill).",
          file=sys.stderr)
    STOP_REQUESTED = True
    terminate_all_children()


def run_chunk(task):
    """Run one summary chunk. Returns
    (chunk_id, n_seeds, out_csv, elapsed, err_or_None)."""
    (chunk_id, n_seeds, sub_filter, start, end, exe, game, tells,
     out_dir, runner) = task
    if STOP_REQUESTED:
        return (chunk_id, n_seeds, None, 0.0, "stopped before start")

    out_dir.mkdir(parents=True, exist_ok=True)
    out_csv = out_dir / "tells.csv"

    cmd = list(runner) + [str(exe), "summary", str(start), str(end),
           "--seed-filter", str(sub_filter),
           "--tells", tells,
           "--game", str(game),
           "--out", str(out_dir)]
    creationflags = 0
    if sys.platform == "win32":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

    t0 = time.perf_counter()
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, creationflags=creationflags)
    except Exception as e:
        return (chunk_id, n_seeds, out_csv,
                time.perf_counter() - t0, f"spawn failed: {e}")

    with POPEN_LOCK:
        POPEN_LIST.append(proc)
    try:
        stdout, stderr = proc.communicate()
    finally:
        with POPEN_LOCK:
            try:    POPEN_LIST.remove(proc)
            except ValueError: pass

    elapsed = time.perf_counter() - t0
    if proc.returncode != 0:
        tail = (stderr or stdout or "")[-1500:]
        return (chunk_id, n_seeds, out_csv, elapsed,
                f"exit={proc.returncode}\n{tail}")
    if not out_csv.exists():
        return (chunk_id, n_seeds, out_csv, elapsed, "no tells.csv produced")
    # Header row + one row per seed expected.
    with open(out_csv, "rb") as f:
        # Cheap line-count via buffer scan.
        lines = sum(buf.count(b"\n") for buf in iter(lambda: f.read(1 << 20), b""))
    expected_rows = 1 + n_seeds  # header + n data rows
    if lines != expected_rows:
        return (chunk_id, n_seeds, out_csv, elapsed,
                f"row mismatch: got {lines}, expected {expected_rows}")
    return (chunk_id, n_seeds, out_csv, elapsed, None)


def main():
    signal.signal(signal.SIGINT, signal_handler)

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("seed_filter", type=Path,
                   help="binary file of uint32 LE seeds, sorted ascending")
    p.add_argument("--tells", required=True,
                   help="comma-separated tell names")
    p.add_argument("--batch", type=int, default=1_000_000,
                   help="seeds per chunk (default: 1,000,000)")
    p.add_argument("--jobs",  type=int, default=8,
                   help="number of parallel workers (default: 8)")
    p.add_argument("--exe",   default=str(DEFAULT_EXE))
    p.add_argument("--game",  default="C:\\Program Files (x86)\\Diablo II")
    p.add_argument("--runner", default="",
                   help="optional command prefix (e.g. 'wine' on Linux)")
    p.add_argument("--out-dir", default="./tells_chunks",
                   help="parent dir for per-chunk subdirs (default: ./tells_chunks)")
    args = p.parse_args()

    if not Path(args.exe).exists():
        p.error(f"mapdump.exe not found at: {args.exe}")
    if not args.seed_filter.exists():
        p.error(f"seed-filter not found: {args.seed_filter}")
    runner = args.runner.split()

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    # Slurp the full seed list into memory (138 M × 4 = 553 MB at peak — fine).
    raw = args.seed_filter.read_bytes()
    if len(raw) % 4 != 0:
        p.error(f"--seed-filter size {len(raw)} not divisible by 4")
    total_seeds = len(raw) // 4

    # Slice into chunks of args.batch seeds. Each chunk's sub-filter file
    # lives in its own subdir alongside the eventual tells.csv.
    n_chunks = (total_seeds + args.batch - 1) // args.batch
    print(f"[*] {total_seeds:,} seeds  /  "
          f"{n_chunks} chunks of {args.batch:,}  /  "
          f"{args.jobs} workers")
    print(f"[*] out_dir: {out_dir}")

    tasks = []
    skipped_existing = 0
    seeds_skipped = 0
    for ci in range(n_chunks):
        s = ci * args.batch * 4
        e = min((ci + 1) * args.batch, total_seeds) * 4
        chunk_bytes = raw[s:e]
        n_seeds = len(chunk_bytes) // 4
        # Min/max seed for the [start, end] bound on the mapdump iteration.
        # Sub-filter is sorted ascending (we trust the global file is too —
        # cheap to verify per-chunk by unpacking the endpoints).
        seed_lo = struct.unpack("<I", chunk_bytes[:4])[0]
        seed_hi = struct.unpack("<I", chunk_bytes[-4:])[0]
        if seed_hi < seed_lo:
            p.error(f"chunk {ci}: seed_hi {seed_hi} < seed_lo {seed_lo} "
                    f"(seed_filter must be sorted ascending)")

        chunk_dir = out_dir / f"chunk_{ci:05d}_{seed_lo:010d}_{seed_hi:010d}"
        sub_filter = chunk_dir / "seeds.bin"
        out_csv = chunk_dir / "tells.csv"

        if out_csv.exists():
            with open(out_csv, "rb") as f:
                lines = sum(buf.count(b"\n")
                            for buf in iter(lambda: f.read(1 << 20), b""))
            if lines == 1 + n_seeds:
                skipped_existing += 1
                seeds_skipped += n_seeds
                continue

        chunk_dir.mkdir(parents=True, exist_ok=True)
        sub_filter.write_bytes(chunk_bytes)

        tasks.append((ci, n_seeds, sub_filter, seed_lo, seed_hi,
                      args.exe, args.game, args.tells, chunk_dir, runner))

    if skipped_existing:
        print(f"[*] {skipped_existing} chunks already complete "
              f"({seeds_skipped:,} seeds); skipping")
    if not tasks:
        print("[+] Nothing to do.")
        return

    work_seeds = total_seeds - seeds_skipped
    semaphore  = threading.BoundedSemaphore(args.jobs)
    bar = tqdm(total=work_seeds, unit="seed", unit_scale=True,
               desc="seeds", smoothing=0.1)

    class State:
        done      = 0
        failed    = []      # (cid, n_seeds, err)
        finished  = 0

    def task_done(future):
        try:
            cid, n_seeds, out_csv, elapsed, err = future.result()
        except Exception as e:
            tqdm.write(f"[err] worker exception: {e}", file=sys.stderr)
            semaphore.release()
            return
        State.finished += 1
        if err:
            State.failed.append((cid, n_seeds, err))
            first = err.splitlines()[0] if err else ""
            tqdm.write(
                f"[err] chunk {cid} ({n_seeds:,} seeds) "
                f"failed in {elapsed:.1f}s: {first}",
                file=sys.stderr)
        else:
            State.done += n_seeds
            rate = n_seeds / elapsed if elapsed > 0 else 0
            bar.set_postfix_str(
                f"chunks {State.finished}/{len(tasks)}, "
                f"last {rate:,.0f} s/s")
        bar.update(n_seeds)
        semaphore.release()

    t_overall = time.perf_counter()
    executor = ThreadPoolExecutor(max_workers=args.jobs)
    try:
        for task in tasks:
            acquired = False
            while not STOP_REQUESTED:
                if semaphore.acquire(timeout=0.25):
                    acquired = True
                    break
            if STOP_REQUESTED:
                if acquired:
                    semaphore.release()
                break
            future = executor.submit(run_chunk, task)
            future.add_done_callback(task_done)
    finally:
        try:
            executor.shutdown(wait=True, cancel_futures=STOP_REQUESTED)
        except TypeError:
            executor.shutdown(wait=True)
        bar.close()

    overall = time.perf_counter() - t_overall
    if STOP_REQUESTED:
        print(f"\n[!] Interrupted.", file=sys.stderr)
    if State.failed:
        print(f"[!] {len(State.failed)} chunk(s) failed.", file=sys.stderr)

    rate = State.done / overall if overall > 0 else 0
    print()
    print(f"[+] Wall clock: {overall:.1f}s")
    print(f"[+] Processed:  {State.done:,} seeds "
          f"({rate:,.0f} seeds/s wall-clock, {args.jobs}x parallel)")
    print(f"[+] Output dir: {out_dir}")

    if State.failed or STOP_REQUESTED:
        sys.exit(1)


if __name__ == "__main__":
    main()
