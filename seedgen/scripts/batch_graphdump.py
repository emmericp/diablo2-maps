#!/usr/bin/env python3
"""Batched parallel level-graph dump.

Splits the seed range into fixed-size batches (default 1M seeds each),
spawns at most --jobs mapdump.exe `levelgraph` processes at a time, and
writes one .bin per batch under --out-dir. No merging — each batch file
is self-contained and indexable: byte offset of seed S inside batch
[bs, be] is (S - bs) * 24.

Resume support: any batch whose output file already exists is skipped.
Ctrl+C stops submitting new batches and sends CTRL_BREAK_EVENT to live
children. A second Ctrl+C escalates to a hard kill.
"""

import argparse
import signal
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


def run_batch(task):
    """Run one levelgraph batch. Returns
    (batch_id, bs, be, out_path, elapsed, err_or_None)."""
    batch_id, bs, be, exe, game, level, out_path, runner = task
    if STOP_REQUESTED:
        return (batch_id, bs, be, out_path, 0.0, "stopped before start")
    # Resume is now decided up-front in main(); we only reach this path
    # for batches that need running. Defensive overwrite on partial files.

    cmd = list(runner) + [str(exe), "levelgraph", str(bs), str(be),
           "--level", str(level),
           "--game", str(game),
           "--outfile", str(out_path)]
    creationflags = 0
    if sys.platform == "win32":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

    t0 = time.perf_counter()
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, creationflags=creationflags)
    except Exception as e:
        return (batch_id, bs, be, out_path,
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
        return (batch_id, bs, be, out_path, elapsed,
                f"exit={proc.returncode}\n{tail}")
    if not out_path.exists():
        return (batch_id, bs, be, out_path, elapsed, "no output file produced")
    expected = (be - bs + 1) * 24
    actual   = out_path.stat().st_size
    if actual != expected:
        return (batch_id, bs, be, out_path, elapsed,
                f"size mismatch: got {actual}, expected {expected}")
    return (batch_id, bs, be, out_path, elapsed, None)


def main():
    signal.signal(signal.SIGINT, signal_handler)

    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("start", type=int, help="first seed (inclusive)")
    p.add_argument("end",   type=int, help="last seed (inclusive)")
    p.add_argument("--level", type=int, required=True,
                   help="level id to export (e.g. 21..24 for tower cellar)")
    p.add_argument("--batch", type=int, default=1_000_000,
                   help="seeds per batch process (default: 1,000,000)")
    p.add_argument("--jobs",  type=int, default=8,
                   help="number of parallel mapdump processes (default: 8)")
    p.add_argument("--exe",   default=str(DEFAULT_EXE))
    p.add_argument("--game",  default="C:\\Program Files (x86)\\Diablo II")
    p.add_argument("--runner", default="",
                   help="optional command prefix for spawning mapdump "
                        "(e.g. 'wine' on Linux). Split on whitespace.")
    p.add_argument("--out-dir", default="./levelgraph_dumps",
                   help="directory for batch .bin files (default: ./levelgraph_dumps)")
    args = p.parse_args()

    if args.end < args.start:
        p.error(f"end ({args.end}) must be >= start ({args.start})")
    if not Path(args.exe).exists():
        p.error(f"mapdump.exe not found at: {args.exe}")
    runner = args.runner.split()

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    seed_ranges = []
    curr = args.start
    while curr <= args.end:
        nxt = min(curr + args.batch - 1, args.end)
        seed_ranges.append((curr, nxt))
        curr = nxt + 1
    total_seeds = args.end - args.start + 1

    print(f"[*] level {args.level}: {total_seeds:,} seeds  /  "
          f"{len(seed_ranges)} batches of {args.batch:,}  /  "
          f"{args.jobs} workers")
    print(f"[*] output dir: {out_dir}")

    tasks = []
    skipped_existing = 0
    seeds_skipped = 0
    for i, (bs, be) in enumerate(seed_ranges):
        out_path = out_dir / f"level{args.level}_{bs:010d}_{be:010d}.bin"
        expected = (be - bs + 1) * 24
        if out_path.exists() and out_path.stat().st_size == expected:
            skipped_existing += 1
            seeds_skipped  += be - bs + 1
            continue
        tasks.append((i, bs, be, args.exe, args.game, args.level, out_path, runner))

    if skipped_existing:
        print(f"[*] {skipped_existing} batches already complete ({seeds_skipped:,} seeds); skipping")
    if not tasks:
        print("[+] Nothing to do.")
        return

    work_seeds = total_seeds - seeds_skipped
    semaphore  = threading.BoundedSemaphore(args.jobs)
    # Progress bar tracks only batches we actually need to run, so the
    # reported rate reflects real work — cached batches are excluded.
    bar = tqdm(total=work_seeds, unit="seed", unit_scale=True,
               desc="seeds", smoothing=0.1)

    class State:
        done      = 0
        failed    = []      # (bid, bs, be, err)
        finished  = 0

    def task_done(future):
        try:
            bid, bs, be, out_path, elapsed, err = future.result()
        except Exception as e:
            tqdm.write(f"[err] worker exception: {e}", file=sys.stderr)
            semaphore.release()
            return
        State.finished += 1
        if err:
            State.failed.append((bid, bs, be, err))
            first = err.splitlines()[0] if err else ""
            tqdm.write(
                f"[err] batch {bid} ({bs:,}..{be:,}) failed in {elapsed:.1f}s: {first}",
                file=sys.stderr)
        else:
            State.done += be - bs + 1
            rate = (be - bs + 1) / elapsed if elapsed > 0 else 0
            bar.set_postfix_str(
                f"batches {State.finished}/{len(seed_ranges)}, "
                f"last {rate:,.0f} s/s")
        bar.update(be - bs + 1)
        semaphore.release()

    t_overall = time.perf_counter()
    executor = ThreadPoolExecutor(max_workers=args.jobs)
    try:
        for task in tasks:
            # Polling-acquire so SIGINT can interrupt us between waits.
            # On Windows, an indefinite semaphore.acquire() blocks the main
            # thread in a C call and the Python signal handler can't run
            # until it returns. A short timeout makes Ctrl+C responsive.
            acquired = False
            while not STOP_REQUESTED:
                if semaphore.acquire(timeout=0.25):
                    acquired = True
                    break
            if STOP_REQUESTED:
                if acquired:
                    semaphore.release()
                break
            future = executor.submit(run_batch, task)
            future.add_done_callback(task_done)
    finally:
        # On normal completion we must NOT cancel pending futures — with low
        # --jobs, a just-submitted task can still be queued (not yet picked
        # up by a worker) when we hit the finally block, and cancel_futures
        # would discard it. Only cancel pending when Ctrl+C was hit.
        try:
            executor.shutdown(wait=True, cancel_futures=STOP_REQUESTED)
        except TypeError:
            executor.shutdown(wait=True)
        bar.close()

    overall = time.perf_counter() - t_overall
    if STOP_REQUESTED:
        print(f"\n[!] Interrupted.", file=sys.stderr)
    if State.failed:
        print(f"[!] {len(State.failed)} batch(es) failed.", file=sys.stderr)

    rate = State.done / overall if overall > 0 else 0
    print()
    print(f"[+] Wall clock: {overall:.1f}s")
    print(f"[+] Processed:  {State.done:,} seeds ({rate:,.0f} seeds/s wall-clock, {args.jobs}x parallel)")
    print(f"[+] Output dir: {out_dir}")

    if State.failed or STOP_REQUESTED:
        sys.exit(1)


if __name__ == "__main__":
    main()
