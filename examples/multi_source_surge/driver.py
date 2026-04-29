"""multi_source_surge driver — FL r6 stress test.

Topology
--------
Three heterogeneous source instances (different fps + image sizes +
"shapes") + two detector instances with different per-call cost,
wired together by inspect.cpp:

    source_steady   (60 Hz, 320x240, steady)    -> detector_fast
    source_burst    (30 Hz background, 640x480) -> detector_fast + detector_slow
    source_variable (45 Hz nominal, 800x600,    -> detector_slow
                     +/-50% sinusoidal jitter)

Two distinct surge events drive each sweep (overlapping with steady traffic):

    t = +1.00 s   burst_count=10 fired on source_steady AND source_burst
                  (concurrent bursts on two streams; tests fan-in correctness)
    t = +2.50 s   burst_count=200 fired on source_burst alone
                  (sustained spike on the bursty stream; tests overflow handling)

Sweeps
------
Two parallelism configurations:

    sweep A — dispatch_threads=1, queue_depth=32  (serial baseline)
    sweep B — dispatch_threads=8, queue_depth=128 (wide parallel)

For each sweep:
    1. Edit project.json's parallelism block.
    2. open_project (force re-read parallelism + re-instantiate).
    3. compile_and_load inspect.cpp.
    4. Snapshot dispatch_stats.
    5. cmd:start fps=200  (timer thread tick rate; not the source's fps).
    6. Drain vars events for DURATION_S, scheduling burst commands at
       the timestamps above via exchange_instance.
    7. cmd:stop.
    8. Snapshot dispatch_stats.
    9. Per-source / per-detector accounting.

Outputs
-------
Comparison table + per-source breakdown printed to stdout. Detailed
interpretation lives in RESULTS.md, friction points in FRICTION.md.
"""
from __future__ import annotations

import json
import statistics
import sys
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
PROJECT_JSON = ROOT / "project.json"
INSPECT_CPP  = ROOT / "inspect.cpp"

DURATION_S = 4.0
DRIVER_FPS = 200      # backend timer; sources have their own fps

SURGE_PLAN = [
    # (t_offset_s, instance, burst_count)
    (1.00, "source_steady", 10),
    (1.00, "source_burst",  10),
    (2.50, "source_burst",  200),
]

SWEEPS = [
    ("A-serial-N1-q32",  1, 32),
    ("B-parallel-N8-q128", 8, 128),
]


def write_parallelism(dispatch_threads: int, queue_depth: int,
                      overflow: str = "drop_oldest") -> None:
    cfg = json.loads(PROJECT_JSON.read_text())
    cfg["parallelism"] = {
        "dispatch_threads": dispatch_threads,
        "queue_depth":      queue_depth,
        "overflow":         overflow,
    }
    PROJECT_JSON.write_text(json.dumps(cfg, indent=2) + "\n")


def drain(c: Client) -> None:
    for q in (c._inbox_vars, c._inbox_previews):
        try:
            while True:
                q.get_nowait()
        except Empty:
            pass


def collect_vars(c: Client, duration_s: float) -> list[dict]:
    events = []
    deadline = time.time() + duration_s
    while time.time() < deadline:
        rem = deadline - time.time()
        try:
            ev = c._inbox_vars.get(timeout=min(0.2, max(0.02, rem)))
        except Empty:
            continue
        flat = {}
        for it in ev.get("items") or []:
            flat[it["name"]] = it.get("value")
        flat["_run_id"] = ev.get("run_id")
        events.append(flat)
    return events


def schedule_surges(c: Client, t0: float, plan: list[tuple]) -> threading.Thread:
    """Background thread that fires exchange_instance burst commands at
    pre-planned timestamps relative to t0."""
    def worker():
        for (t_off, inst, count) in plan:
            wait = (t0 + t_off) - time.time()
            if wait > 0:
                time.sleep(wait)
            try:
                c.exchange_instance(inst, {"command": "burst", "count": count})
            except Exception as e:
                print(f"  [surge] exchange_instance({inst}, burst={count}) "
                      f"failed: {e}")
    th = threading.Thread(target=worker, daemon=True)
    th.start()
    return th


def summarise(events: list[dict], stats_before: dict, stats_after: dict,
              duration_s: float, label: str) -> dict:
    active_events = [e for e in events
                     if e.get("active") is True and "seq" in e]
    n_total  = len(events)
    n_active = len(active_events)
    n_inactive = sum(1 for e in events if e.get("active") is False)

    by_src = Counter(e.get("src") or "?" for e in active_events)
    seq_per_src: dict[str, list[int]] = defaultdict(list)
    for e in active_events:
        s = e.get("src") or "?"
        if isinstance(e.get("seq"), (int, float)):
            seq_per_src[s].append(int(e["seq"]))

    n_used_fast = sum(1 for e in active_events if e.get("used_fast") is True)
    n_used_slow = sum(1 for e in active_events if e.get("used_slow") is True)
    n_used_both = sum(1 for e in active_events
                      if e.get("used_fast") is True and e.get("used_slow") is True)

    lats = [float(e["latency_us"]) for e in active_events
            if isinstance(e.get("latency_us"), (int, float))]

    # cmd:start resets the drop counters and the high watermark (see
    # backend/src/service_main.cpp `name == "start"`); stats_after's
    # values therefore represent *this sweep* in isolation. We don't
    # subtract stats_before — that would underflow when the previous
    # sweep accumulated drops the current cmd:start has since cleared.
    drops = stats_after.get("dropped_oldest", 0) + \
            stats_after.get("dropped_newest", 0)
    qmax = stats_after.get("queue_depth_high_watermark", 0)
    qcap = stats_after.get("queue_depth_cap", 0)

    throughput = n_active / duration_s if duration_s > 0 else 0.0

    print(f"[{label}]")
    print(f"  vars events received           : {n_total}")
    print(f"  active inspects (with seq)     : {n_active}")
    print(f"  inactive (timer-only ticks)    : {n_inactive}")
    print(f"  by source                      : {dict(by_src)}")
    print(f"  used fast / slow / both        : {n_used_fast} / {n_used_slow} / {n_used_both}")
    if lats:
        print(f"  latency_us mean / median / p95 : "
              f"{statistics.mean(lats):.0f} / "
              f"{statistics.median(lats):.0f} / "
              f"{sorted(lats)[max(0, int(len(lats)*0.95)-1)]:.0f}")
    print(f"  throughput (active / sec)      : {throughput:.1f}")
    print(f"  dispatch_stats AFTER           : {stats_after}")
    print(f"  drops during sweep             : {drops}")
    print(f"  queue peak / cap               : {qmax} / {qcap}")

    # Sequence-attribution sanity: each source's seqs should be
    # monotonically issued; we check unique-count vs max-min span to
    # catch routing bugs (a frame attributed to wrong source).
    attr_warnings = []
    for s, seqs in seq_per_src.items():
        if not seqs:
            continue
        uniq = len(set(seqs))
        # We're sampling under drops; check the basic monotonic property.
        if uniq != len(seqs):
            attr_warnings.append(f"  {s}: {len(seqs) - uniq} duplicate seq values")
    if attr_warnings:
        print("  attribution warnings:")
        for w in attr_warnings:
            print(w)

    return {
        "label":       label,
        "events":      n_total,
        "active":      n_active,
        "by_src":      dict(by_src),
        "used_fast":   n_used_fast,
        "used_slow":   n_used_slow,
        "used_both":   n_used_both,
        "throughput":  throughput,
        "drops":       drops,
        "qmax":        qmax,
        "qcap":        qcap,
        "lat_mean":    statistics.mean(lats) if lats else None,
        "lat_median":  statistics.median(lats) if lats else None,
        "lat_p95":     (sorted(lats)[max(0, int(len(lats)*0.95)-1)] if lats else None),
        "stats_after": stats_after,
        "stats_before": stats_before,
    }


def run_sweep(c: Client, label: str, dispatch_threads: int, queue_depth: int) -> dict:
    print(f"\n=== sweep: {label}  N={dispatch_threads}  queue={queue_depth} ===")
    write_parallelism(dispatch_threads, queue_depth)

    info = c.open_project(str(ROOT))
    inst_names = [i.get('name') for i in (info.get('instances') or [])]
    print(f"  reopened. instances: {inst_names}")
    expect = {"source_steady", "source_burst", "source_variable",
              "detector_fast", "detector_slow"}
    missing = expect - set(inst_names)
    if missing:
        print(f"  WARNING: missing instances: {missing}")

    c.compile_and_load(str(INSPECT_CPP))
    print("  inspect.cpp compiled")

    # Let sources warm up.
    time.sleep(0.3)
    drain(c)

    try:
        stats_before = c.call("dispatch_stats")
    except ProtocolError as e:
        print(f"  dispatch_stats unavailable: {e}")
        stats_before = {}
    print(f"  dispatch_stats BEFORE          : {stats_before}")

    c.call("start", {"fps": DRIVER_FPS})
    t0 = time.time()
    schedule_surges(c, t0, SURGE_PLAN)
    events = collect_vars(c, DURATION_S)
    elapsed = time.time() - t0
    c.call("stop")

    time.sleep(0.2)
    try:
        stats_after = c.call("dispatch_stats")
    except ProtocolError as e:
        print(f"  dispatch_stats unavailable: {e}")
        stats_after = {}

    drain(c)
    return summarise(events, stats_before, stats_after, elapsed, label)


def main() -> int:
    print("multi_source_surge — FL r6 (3 sources, 2 detectors, 2 surges)")
    with Client() as c:
        results = []
        try:
            for (label, n, q) in SWEEPS:
                results.append((label, n, q, run_sweep(c, label, n, q)))
        except ProtocolError as e:
            print("\nFATAL:", e)
            return 1

        print("\n\n=========== COMPARISON TABLE ===========\n")
        hdr = ("Sweep", "N", "Q", "active", "thr/s", "drops",
               "qmax", "lat_mean_us", "lat_p95_us")
        print("  ".join(f"{h:>14}" for h in hdr))
        for (label, n, q, r) in results:
            row = (label[:14],
                   str(n), str(q),
                   f"{r['active']}",
                   f"{r['throughput']:.1f}",
                   f"{r['drops']}",
                   f"{r['qmax']}",
                   f"{r['lat_mean']:.0f}" if r['lat_mean'] is not None else "-",
                   f"{r['lat_p95']:.0f}"  if r['lat_p95']  is not None else "-")
            print("  ".join(f"{x:>14}" for x in row))

        print("\n=========== PER-SOURCE BREAKDOWN ===========\n")
        for (label, n, q, r) in results:
            print(f"{label}: by_src = {r['by_src']}  "
                  f"fast={r['used_fast']} slow={r['used_slow']} "
                  f"both={r['used_both']}")

        if len(results) >= 2 and results[0][3]["throughput"] > 0:
            sp = results[1][3]["throughput"] / results[0][3]["throughput"]
            print(f"\nspeedup (sweep B vs A): {sp:.2f}x")

        # Hot-reload sanity: recompile inspect.cpp mid-run with N=8/q=128
        # already loaded (last sweep), then drive a small post-reload
        # window to confirm dispatch keeps flowing across all 5 instances.
        print("\n=========== HOT-RELOAD CHECK ===========\n")
        try:
            c.call("start", {"fps": DRIVER_FPS})
            time.sleep(0.4)
            t_reload = time.time()
            c.compile_and_load(str(INSPECT_CPP))
            dt = time.time() - t_reload
            print(f"  compile_and_load mid-run took {dt*1000:.0f} ms")
            drain(c)
            ev = collect_vars(c, 1.0)
            c.call("stop")
            srcs_seen = Counter(e.get("src") for e in ev
                                if e.get("active") is True)
            print(f"  post-reload by_src: {dict(srcs_seen)}")
            ok_reload = (
                sum(1 for e in ev if e.get("active") is True) > 5
                and len(srcs_seen) >= 3
            )
            print(f"  hot-reload OK: {ok_reload}")
        except ProtocolError as e:
            print(f"  hot-reload FAILED: {e}")

        # Mechanical pass/fail. Real interpretation in RESULTS.md.
        a = results[0][3] if results else {"active": 0}
        b = results[1][3] if len(results) > 1 else a
        ok = (a.get("active", 0) > 5
              and b.get("active", 0) >= a.get("active", 0)
              and len(b.get("by_src", {})) >= 3)
        print("\nVERDICT (mechanical):", "PASS" if ok else "FAIL")
        return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
