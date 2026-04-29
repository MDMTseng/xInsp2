"""burst_pipeline driver — observe parallelism trade-offs.

Three sweeps:
  1. dispatch_threads=1, queue_depth=32  (baseline serial)
  2. dispatch_threads=4, queue_depth=32  (parallel)
  3. dispatch_threads=4, queue_depth=4   (parallel + tight queue)

For each sweep:
  - Edit project.json to set the parallelism block.
  - open_project (forces backend to re-read parallelism config).
  - compile_and_load inspect.cpp.
  - Snapshot dispatch_stats (baseline).
  - cmd:start fps=60.
  - Drain vars events for 2.0 s.
  - Snapshot dispatch_stats (final).
  - cmd:stop, drain residue.
  - Print per-sweep summary.

Compute throughput, drops, mean/median latency_us, then a comparison
table at the end.
"""
from __future__ import annotations

import json
import statistics
import sys
import time
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
PROJECT_JSON = ROOT / "project.json"
INSPECT_CPP  = ROOT / "inspect.cpp"

DURATION_S = 2.0
FPS        = 60


def write_parallelism(dispatch_threads: int, queue_depth: int,
                      overflow: str = "drop_oldest") -> None:
    cfg = json.loads(PROJECT_JSON.read_text())
    cfg["parallelism"] = {
        "dispatch_threads": dispatch_threads,
        "queue_depth":      queue_depth,
        "overflow":         overflow,
    }
    PROJECT_JSON.write_text(json.dumps(cfg, indent=2) + "\n")


def collect_vars(c: Client, duration_s: float) -> list[dict]:
    events = []
    deadline = time.time() + duration_s
    while time.time() < deadline:
        rem = deadline - time.time()
        try:
            ev = c._inbox_vars.get(timeout=min(0.5, max(0.05, rem)))
        except Empty:
            continue
        flat = {}
        for it in ev.get("items") or []:
            flat[it["name"]] = it.get("value")
        flat["_run_id"] = ev.get("run_id")
        events.append(flat)
    return events


def drain(c: Client) -> None:
    for q in (c._inbox_vars, c._inbox_previews):
        try:
            while True:
                q.get_nowait()
        except Empty:
            pass


def summarise(events: list[dict], stats_before: dict, stats_after: dict,
              duration_s: float, label: str) -> dict:
    active_events = [e for e in events if e.get("active") is True
                                          and "seq" in e]
    n_active = len(active_events)
    n_total  = len(events)
    n_inactive = sum(1 for e in events if e.get("active") is False)

    seqs = [int(e["seq"]) for e in active_events
            if isinstance(e.get("seq"), (int, float))]
    seqs_sorted = sorted(set(seqs))

    lats = [float(e["latency_us"]) for e in active_events
            if isinstance(e.get("latency_us"), (int, float))]

    drops_before = stats_before.get("dropped_oldest", 0) + \
                   stats_before.get("dropped_newest", 0)
    drops_after  = stats_after.get("dropped_oldest", 0) + \
                   stats_after.get("dropped_newest", 0)
    drops = drops_after - drops_before
    qmax = stats_after.get("queue_depth_high_watermark", 0)
    qcap = stats_after.get("queue_depth_cap", 0)

    throughput = n_active / duration_s if duration_s > 0 else 0.0

    print(f"[{label}]")
    print(f"  vars events received           : {n_total}")
    print(f"  active inspects (with seq)     : {n_active}")
    print(f"  inactive (timer-only ticks)    : {n_inactive}")
    print(f"  unique seq values              : {len(seqs_sorted)}")
    if seqs_sorted:
        print(f"  seq range                      : {seqs_sorted[0]}..{seqs_sorted[-1]}")
    if lats:
        print(f"  latency_us mean / median / p95 : "
              f"{statistics.mean(lats):.0f} / "
              f"{statistics.median(lats):.0f} / "
              f"{sorted(lats)[int(len(lats)*0.95)-1]:.0f}")
    print(f"  throughput (active / sec)      : {throughput:.1f}")
    print(f"  dispatch_stats AFTER           : {stats_after}")
    print(f"  drops during sweep             : {drops}")
    print(f"  queue peak / cap               : {qmax} / {qcap}")

    return {
        "label":       label,
        "events":      n_total,
        "active":      n_active,
        "throughput":  throughput,
        "drops":       drops,
        "qmax":        qmax,
        "lat_mean":    statistics.mean(lats) if lats else None,
        "lat_median":  statistics.median(lats) if lats else None,
        "lat_p95":     (sorted(lats)[int(len(lats)*0.95)-1] if lats else None),
        "stats_after": stats_after,
        "stats_before": stats_before,
    }


def run_sweep(c: Client, label: str, dispatch_threads: int, queue_depth: int) -> dict:
    print(f"\n=== sweep: {label}  N={dispatch_threads}  queue={queue_depth} ===")
    write_parallelism(dispatch_threads, queue_depth)

    info = c.open_project(str(ROOT))
    print(f"  reopened. instances: "
          f"{[i.get('name') for i in (info.get('instances') or [])]}")

    c.compile_and_load(str(INSPECT_CPP))
    print("  inspect.cpp compiled")

    # Let the source warm up before starting (it auto-runs at instance
    # ctor; give it ~200 ms to push frames into the bus).
    time.sleep(0.2)
    drain(c)

    try:
        stats_before = c.call("dispatch_stats")
    except ProtocolError as e:
        print(f"  dispatch_stats unavailable: {e}")
        stats_before = {}
    print(f"  dispatch_stats BEFORE          : {stats_before}")

    c.call("start", {"fps": FPS})
    t0 = time.time()
    events = collect_vars(c, DURATION_S)
    elapsed = time.time() - t0
    c.call("stop")

    # Give in-flight inspects a beat to settle (max inspect is ~35 ms).
    time.sleep(0.1)
    try:
        stats_after = c.call("dispatch_stats")
    except ProtocolError as e:
        print(f"  dispatch_stats unavailable: {e}")
        stats_after = {}

    drain(c)
    return summarise(events, stats_before, stats_after, elapsed, label)


def main() -> int:
    print("burst_pipeline FL round 5 — three-sweep parallelism observation")
    with Client() as c:
        # First sweep also opens the project for the first time. The
        # project.json content is already at N=4 q=32 from authoring;
        # write it explicitly anyway so re-runs are deterministic.
        try:
            r1 = run_sweep(c, "1-baseline-serial",  dispatch_threads=1, queue_depth=32)
            r2 = run_sweep(c, "2-parallel-N4",      dispatch_threads=4, queue_depth=32)
            r3 = run_sweep(c, "3-parallel-tight-q", dispatch_threads=4, queue_depth=4)
        except ProtocolError as e:
            print("\nFATAL:", e)
            return 1

        print("\n\n=========== COMPARISON TABLE ===========\n")
        hdr = ("Sweep", "N", "Q", "active/2s", "thr/s", "drops",
               "qmax", "lat_mean_us", "lat_p95_us")
        print("  ".join(f"{h:>14}" for h in hdr))
        for r, (n, q) in [(r1, (1, 32)), (r2, (4, 32)), (r3, (4, 4))]:
            row = (r["label"][:14],
                   str(n), str(q),
                   f"{r['active']}",
                   f"{r['throughput']:.1f}",
                   f"{r['drops']}",
                   f"{r['qmax']}",
                   f"{r['lat_mean']:.0f}" if r['lat_mean'] is not None else "-",
                   f"{r['lat_p95']:.0f}"  if r['lat_p95']  is not None else "-")
            print("  ".join(f"{x:>14}" for x in row))

        if r1["throughput"] > 0:
            speedup = r2["throughput"] / r1["throughput"]
            print(f"\nspeedup (sweep 2 vs 1): {speedup:.2f}x")
        else:
            speedup = 0.0
            print("\nspeedup: N/A (baseline throughput was 0)")

        # Lightweight pass/fail. Real interpretation is in RESULTS.md.
        ok = r1["active"] > 5 and r2["active"] > r1["active"]
        print("\nVERDICT (mechanical):", "PASS" if ok else "FAIL")
        return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
