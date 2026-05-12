#!/usr/bin/env python3
"""Batched D2 seed dumping. Spawns one mapdump.exe per chunk in parallel,
merges per-chunk CSVs at the end. D2's LoadAct leak resets per subprocess,
so per-seed throughput stays flat regardless of total range.
"""

import argparse
import shutil
import signal
import subprocess
import sys
import tempfile
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

# Shared state accessed by worker threads and the signal handler.
STOP_REQUESTED = False
POPEN_LIST     = []                       # live subprocess.Popen instances
POPEN_LOCK     = threading.Lock()


def terminate_all_children(force=False):
    """Send a stop signal to every still-running mapdump.exe.
    On Windows we use CTRL_BREAK_EVENT (default action: process exits);
    `force` upgrades to a hard kill."""
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
    """Ctrl+C: stop submitting, terminate running children, but keep
    whatever chunks already finished so Phase 2 can merge them.
    Second Ctrl+C escalates to kill -9."""
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
    """Run one mapdump.exe chunk. Returns
    (chunk_id, cs, ce, csv_path_or_None, matched_rows, elapsed, err_or_None)."""
    chunk_id, cs, ce, exe, game, chunk_dir, tells, tell_levels, filters, seed_filter = task
    if STOP_REQUESTED:
        return (chunk_id, cs, ce, None, 0, 0.0, "stopped before start")

    cmd = [str(exe), "summary", str(cs), str(ce),
           "--game", str(game), "--out", str(chunk_dir)]
    if tells:
        cmd += ["--tells", tells]
    if tell_levels:
        cmd += ["--tell-levels", tell_levels]
    for f in filters:
        cmd += ["--filter", f]
    if seed_filter:
        cmd += ["--seed-filter", str(seed_filter)]

    # CREATE_NEW_PROCESS_GROUP so CTRL_BREAK_EVENT targets only this child.
    creationflags = 0
    if sys.platform == "win32":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

    t0 = time.perf_counter()
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, creationflags=creationflags)
    except Exception as e:
        return (chunk_id, cs, ce, None, 0,
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
        return (chunk_id, cs, ce, None, 0, elapsed,
                f"exit={proc.returncode}\n{tail}")

    csv_path = chunk_dir / "tells.csv"
    if not csv_path.exists():
        return (chunk_id, cs, ce, None, 0, elapsed, "no CSV produced")

    try:
        with open(csv_path, "r", encoding="utf-8") as f:
            matched = max(0, sum(1 for _ in f) - 1)
    except OSError:
        matched = 0
    return (chunk_id, cs, ce, csv_path, matched, elapsed, None)


def main():
    signal.signal(signal.SIGINT, signal_handler)

    p = argparse.ArgumentParser(
        description="Batched parallel D2 seed dumping with interruptible Ctrl+C.")
    p.add_argument("start", type=int)
    p.add_argument("end",   type=int)
    p.add_argument("--tells", default="",
                   help="comma list of tell names (default: all)")
    p.add_argument("--tell-levels", default="",
                   help="comma list of level ids; restrict tells to those touching any of them")
    p.add_argument("--filter", action="append", default=[],
                   help="K=V[,V] (repeatable)")
    p.add_argument("--seed-filter", default=None,
                   help="binary file of uint32 LE seeds; only these are processed")
    p.add_argument("--chunk", type=int, default=10000,
                   help="seeds per worker process (default: 10000)")
    p.add_argument("--jobs",  type=int, default=8,
                   help="parallel workers (default: 8)")
    p.add_argument("--exe",  default=str(DEFAULT_EXE))
    p.add_argument("--game", default="C:\\Program Files (x86)\\Diablo II")
    p.add_argument("--out",  default="tells.csv")
    p.add_argument("--tmp-dir", default=None,
                   help="reuse this tmp root (default: fresh tempfile.mkdtemp())")
    args = p.parse_args()

    if args.end < args.start:
        p.error(f"end ({args.end}) must be >= start ({args.start})")
    if not Path(args.exe).exists():
        p.error(f"mapdump.exe not found at: {args.exe}")
    seed_filter = None
    if args.seed_filter:
        seed_filter = Path(args.seed_filter).resolve()
        if not seed_filter.exists():
            p.error(f"--seed-filter file not found: {seed_filter}")

    # Track only paths we ourselves create so cleanup never touches anything
    # pre-existing under --tmp-dir.
    created_paths = []

    if args.tmp_dir:
        temp_root = Path(args.tmp_dir).resolve()
        if not temp_root.exists():
            temp_root.mkdir(parents=True)
            created_paths.append(temp_root)
    else:
        temp_root = Path(tempfile.mkdtemp(prefix="mapdump_batch_"))
        created_paths.append(temp_root)

    seed_ranges = []
    curr = args.start
    while curr <= args.end:
        nxt = min(curr + args.chunk - 1, args.end)
        seed_ranges.append((curr, nxt))
        curr = nxt + 1
    total_seeds = args.end - args.start + 1

    print(f"[*] {total_seeds:,} seeds  /  {len(seed_ranges)} chunks of {args.chunk:,}  /  {args.jobs} workers")
    if args.tells:       print(f"[*] tells:  {args.tells}")
    if args.tell_levels: print(f"[*] tell-levels: {args.tell_levels}")
    if args.filter:      print(f"[*] filter: {args.filter}")
    if seed_filter:      print(f"[*] seed-filter: {seed_filter}")

    tasks = []
    for i, (cs, ce) in enumerate(seed_ranges):
        chunk_dir = temp_root / f"chunk_{i:05d}"
        chunk_dir.mkdir(exist_ok=False)
        created_paths.append(chunk_dir)
        tasks.append((i, cs, ce, args.exe, args.game, chunk_dir,
                      args.tells, args.tell_levels, args.filter, seed_filter))

    # Mutable per-run counters, updated from worker-thread callbacks.
    class State:
        matched     = 0
        chunk_times = []
        failures    = []          # list of (cid, cs, ce, err)
        results     = {}          # cid -> csv_path

    semaphore = threading.BoundedSemaphore(args.jobs)
    bar = tqdm(total=total_seeds, unit="seed", unit_scale=True,
               desc="seeds", smoothing=0.1)

    def task_done(future):
        try:
            cid, cs, ce, csv_path, matched, elapsed, err = future.result()
        except Exception as e:
            tqdm.write(f"[err] worker exception: {e}", file=sys.stderr)
            semaphore.release()
            return

        if err:
            State.failures.append((cid, cs, ce, err))
            first = err.splitlines()[0] if err else ""
            tqdm.write(
                f"[err] chunk {cid} ({cs:,}..{ce:,}) failed in {elapsed:.1f}s: {first}",
                file=sys.stderr)
        else:
            State.results[cid] = csv_path
            State.matched     += matched
            State.chunk_times.append(elapsed)
            bar.set_postfix_str(
                f"chunks {len(State.results)}/{len(seed_ranges)}, "
                f"+{matched} match, {(ce - cs + 1)/elapsed:.0f} s/s")
        bar.update(ce - cs + 1)
        semaphore.release()

    t_overall = time.perf_counter()
    executor = ThreadPoolExecutor(max_workers=args.jobs)
    try:
        for task in tasks:
            if STOP_REQUESTED:
                break
            semaphore.acquire()
            if STOP_REQUESTED:
                semaphore.release()
                break
            future = executor.submit(run_chunk, task)
            future.add_done_callback(task_done)
    finally:
        executor.shutdown(wait=True)
        bar.close()

    overall = time.perf_counter() - t_overall

    if STOP_REQUESTED:
        print(f"\n[!] Interrupted. {len(State.results)} chunk(s) completed before stop.",
              file=sys.stderr)
    if State.failures:
        print(f"[!] {len(State.failures)} chunk(s) failed.", file=sys.stderr)

    # Phase 2: merge successful chunks (in chunk-id order so output is in
    # ascending seed order despite parallel completion).
    out_path = Path(args.out).resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if State.results:
        sorted_ids = sorted(State.results.keys())
        with open(out_path, "w", encoding="utf-8", newline="") as out_file:
            first = True
            for cid in tqdm(sorted_ids, unit="file", desc="merging"):
                with open(State.results[cid], "r", encoding="utf-8") as in_f:
                    header = in_f.readline()
                    if first:
                        out_file.write(header)
                        first = False
                    shutil.copyfileobj(in_f, out_file)
    else:
        open(out_path, "w").close()

    # Wipe only paths we created. Reverse order so chunk dirs go before temp_root.
    for path in reversed(created_paths):
        try:
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            elif path.is_file():
                path.unlink(missing_ok=True)
        except Exception:
            pass

    # Summary.
    successful_seeds = sum(seed_ranges[cid][1] - seed_ranges[cid][0] + 1
                           for cid in State.results)
    rate = successful_seeds / overall if overall > 0 else 0
    print()
    print(f"[+] Wall clock: {overall:.1f}s, {len(State.results)}/{len(seed_ranges)} chunks ok")
    print(f"[+] Processed:  {successful_seeds:,} seeds ({rate:,.0f} seeds/s wall-clock, {args.jobs}x parallel)")
    print(f"[+] Matched:    {State.matched:,}    Skipped: {successful_seeds - State.matched:,}")
    if State.chunk_times:
        ct = State.chunk_times
        print(f"[+] Chunk time: mean {sum(ct)/len(ct):.1f}s, min {min(ct):.1f}s, max {max(ct):.1f}s")
    print(f"[+] Output:     {out_path}")

    if State.failures or STOP_REQUESTED:
        sys.exit(1)


if __name__ == "__main__":
    main()
