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

Measurement
-----------
The per-event VAR model was removed from core, so throughput and
attribution are gathered two generic ways:

  * Throughput  — the backend emits a `run_finished` lifecycle event per
    dispatched inspect; we count those over the sweep window (generic
    client events inbox).
  * Fan-in / routing — inspect.cpp pushes a tiny record per active
    inspect to the `expose` sink (channel "runs") carrying src + which
    detector(s) ran. We subscribe to "runs" and decode the XEX1 frames
    via qa/lib/xex1.py to confirm every source got routed.

Outputs
-------
Comparison table + per-source breakdown printed to stdout. Detailed
interpretation lives in RESULTS.md, friction points in FRICTION.md.
"""
from __future__ import annotations

import json
import sys
import threading
import time
from collections import Counter
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xex1 import collect_frames, subscribe   # noqa: E402

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
    """Clear lifecycle events and any queued binary (expose) frames."""
    try:
        while True:
            c._inbox_events.get_nowait()
    except Empty:
        pass
    c.drain_binary()


def collect_window(c: Client, duration_s: float) -> tuple[int, list[dict]]:
    """Run the sweep window: count `run_finished` events for throughput and
    decode the `expose` "runs" frames for per-source attribution.

    Returns (run_finished_count, frames) where each frame is the decoded
    XEX1 record {channel, seq, values, images}.
    """
    finished = 0
    frames: list[dict] = []
    deadline = time.time() + duration_s
    while time.time() < deadline:
        rem = deadline - time.time()
        try:
            ev = c._inbox_events.get(timeout=min(0.1, max(0.02, rem)))
        except Empty:
            ev = None
        if ev is not None and ev.get("name") == "run_finished":
            finished += 1
        # Drain whatever expose frames have arrived so far.
        for fr in collect_frames(c):
            if fr.get("channel") == "runs":
                frames.append(fr)
    # Catch any trailing frames.
    for fr in collect_frames(c):
        if fr.get("channel") == "runs":
            frames.append(fr)
    return finished, frames


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


def summarise(finished: int, frames: list[dict], stats_after: dict,
              duration_s: float, label: str) -> dict:
    """Reduce the run_finished count + decoded expose frames into the
    sweep's headline metrics. `active` == number of expose frames, since
    each active inspect pushes exactly one."""
    values = [fr.get("values") or {} for fr in frames]
    n_active = len(values)

    by_src = Counter(v.get("src") or "?" for v in values)
    seq_per_src: dict[str, list[int]] = {}
    for v in values:
        s = v.get("src") or "?"
        if isinstance(v.get("seq"), (int, float)):
            seq_per_src.setdefault(s, []).append(int(v["seq"]))

    n_used_fast = sum(1 for v in values if v.get("used_fast") is True)
    n_used_slow = sum(1 for v in values if v.get("used_slow") is True)
    n_used_both = sum(1 for v in values
                      if v.get("used_fast") is True and v.get("used_slow") is True)

    # cmd:start resets the drop counters and the high watermark (see
    # backend/src/service_main.cpp `name == "start"`); stats_after's
    # values therefore represent *this sweep* in isolation. The real
    # dispatch_stats fields are `dropped` (single overflow counter) and
    # `queue_depth_high_watermark` / `queue_depth_cap`.
    drops = stats_after.get("dropped", 0) or 0
    qmax = stats_after.get("queue_depth_high_watermark", 0) or 0
    qcap = stats_after.get("queue_depth_cap", 0) or 0

    throughput = finished / duration_s if duration_s > 0 else 0.0

    print(f"[{label}]")
    print(f"  run_finished events (window)   : {finished}")
    print(f"  active inspects (expose frames): {n_active}")
    print(f"  by source                      : {dict(by_src)}")
    print(f"  used fast / slow / both        : {n_used_fast} / {n_used_slow} / {n_used_both}")
    print(f"  throughput (run_finished / sec): {throughput:.1f}")
    print(f"  dispatch_stats AFTER           : {stats_after}")
    print(f"  drops during sweep             : {drops}")
    print(f"  queue peak / cap               : {qmax} / {qcap}")

    # Sequence-attribution sanity: each source's seqs should be unique
    # within the window; duplicates would point at a routing/fan-in bug.
    attr_warnings = []
    for s, seqs in seq_per_src.items():
        if not seqs:
            continue
        uniq = len(set(seqs))
        if uniq != len(seqs):
            attr_warnings.append(f"  {s}: {len(seqs) - uniq} duplicate seq values")
    if attr_warnings:
        print("  attribution warnings:")
        for w in attr_warnings:
            print(w)

    return {
        "label":       label,
        "finished":    finished,
        "active":      n_active,
        "by_src":      dict(by_src),
        "used_fast":   n_used_fast,
        "used_slow":   n_used_slow,
        "used_both":   n_used_both,
        "throughput":  throughput,
        "drops":       drops,
        "qmax":        qmax,
        "qcap":        qcap,
        "stats_after": stats_after,
    }


def run_sweep(c: Client, label: str, dispatch_threads: int, queue_depth: int) -> dict:
    print(f"\n=== sweep: {label}  N={dispatch_threads}  queue={queue_depth} ===")
    write_parallelism(dispatch_threads, queue_depth)

    info = c.open_project(str(ROOT))
    inst_names = [i.get('name') for i in (info.get('instances') or [])]
    print(f"  reopened. instances: {inst_names}")
    expect = {"source_steady", "source_burst", "source_variable",
              "detector_fast", "detector_slow", "expose"}
    missing = expect - set(inst_names)
    if missing:
        print(f"  WARNING: missing instances: {missing}")

    c.compile_and_load(str(INSPECT_CPP))
    print("  inspect.cpp compiled")

    # Subscribe to the expose channel so the sink pushes per-inspect
    # frames (subscription is reset by open_project — re-arm every sweep).
    subscribe(c, ["runs"])

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
    finished, frames = collect_window(c, DURATION_S)
    elapsed = time.time() - t0
    c.call("stop")

    time.sleep(0.2)
    try:
        stats_after = c.call("dispatch_stats")
    except ProtocolError as e:
        print(f"  dispatch_stats unavailable: {e}")
        stats_after = {}

    drain(c)
    return summarise(finished, frames, stats_after, elapsed, label)


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
               "qmax", "fast", "slow", "both")
        print("  ".join(f"{h:>14}" for h in hdr))
        for (label, n, q, r) in results:
            row = (label[:14],
                   str(n), str(q),
                   f"{r['active']}",
                   f"{r['throughput']:.1f}",
                   f"{r['drops']}",
                   f"{r['qmax']}",
                   f"{r['used_fast']}",
                   f"{r['used_slow']}",
                   f"{r['used_both']}")
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
        # window to confirm dispatch keeps flowing across all sources.
        print("\n=========== HOT-RELOAD CHECK ===========\n")
        try:
            subscribe(c, ["runs"])
            c.call("start", {"fps": DRIVER_FPS})
            time.sleep(0.4)
            t_reload = time.time()
            c.compile_and_load(str(INSPECT_CPP))
            dt = time.time() - t_reload
            print(f"  compile_and_load mid-run took {dt*1000:.0f} ms")
            subscribe(c, ["runs"])   # re-arm: compile may reset subscription
            drain(c)
            _finished, frames = collect_window(c, 1.0)
            c.call("stop")
            srcs_seen = Counter((fr.get("values") or {}).get("src")
                                for fr in frames)
            print(f"  post-reload by_src: {dict(srcs_seen)}")
            ok_reload = (len(frames) > 5 and len(srcs_seen) >= 3)
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
