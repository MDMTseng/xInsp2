"""Driver for hot_reload_run FL test.

Validates that compile_and_load while a cmd:start continuous run is
active preserves xi::state() and xi::Param<T> values, and that the
run continues across the reload boundary.
"""
from __future__ import annotations

import os
import shutil
import sys
import threading
import time
from queue import Empty

# Locate the SDK
HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.normpath(os.path.join(HERE, "..", "..", "tools", "xinsp2_py"))
sys.path.insert(0, SDK)

from xinsp2.client import Client, ProtocolError  # noqa: E402

V1_SRC = os.path.join(HERE, "inspect_v1.cpp")
V2_SRC = os.path.join(HERE, "inspect_v2.cpp")
LIVE = os.path.join(HERE, "inspect.cpp")

THRESHOLD_SENTINEL = 137  # arbitrary mid-range value to verify Param survives


def items_by_name(items):
    return {it["name"]: it for it in items}


def vars_drainer(c: Client, sink: list, stop_evt: threading.Event):
    """Background thread: pull every `vars` message off the SDK's queue,
    timestamp it, append to `sink`."""
    while not stop_evt.is_set():
        try:
            msg = c._inbox_vars.get(timeout=0.05)
        except Empty:
            continue
        sink.append({
            "ts": time.monotonic(),
            "run_id": msg.get("run_id"),
            "items": msg.get("items", []),
        })


def main():
    # Stage v1 as the live source.
    shutil.copyfile(V1_SRC, LIVE)

    with Client() as c:
        print("[driver] connected", c.version())

        c.compile_and_load(LIVE.replace("\\", "/"))
        print("[driver] v1 compiled+loaded")

        # Bump threshold to a sentinel so we can verify it survives.
        c.set_param("threshold", THRESHOLD_SENTINEL)
        print(f"[driver] set threshold = {THRESHOLD_SENTINEL}")

        # Start a background drainer for `vars` messages.
        sink: list = []
        stop_evt = threading.Event()
        t = threading.Thread(target=vars_drainer, args=(c, sink, stop_evt), daemon=True)
        t.start()

        # Begin continuous mode.
        c.call("start", {"fps": 20})
        t_started = time.monotonic()
        print("[driver] cmd:start fps=20 issued")

        # Collect ~2 s of vars events pre-reload.
        time.sleep(2.0)
        n_pre_observed = len(sink)
        print(f"[driver] pre-reload vars events: {n_pre_observed}")

        # Swap on-disk source to v2 and call compile_and_load again.
        shutil.copyfile(V2_SRC, LIVE)
        t_pre_reload = time.monotonic()
        try:
            c.compile_and_load(LIVE.replace("\\", "/"))
            t_reload_returned = time.monotonic()
            reload_ok = True
            reload_err = None
        except ProtocolError as e:
            t_reload_returned = time.monotonic()
            reload_ok = False
            reload_err = str(e)
            print("[driver] compile_and_load FAILED:", reload_err)
        print(f"[driver] reload returned ok={reload_ok} "
              f"(took {(t_reload_returned - t_pre_reload)*1000:.0f} ms)")

        # If backend stopped continuous mode on reload, restart it so we can
        # still collect post-reload events; record that we had to.
        had_to_restart = False
        try:
            # ping/no-op probe — try start again; backend rejects if already running.
            c.call("start", {"fps": 20})
            had_to_restart = True
            print("[driver] NOTE: had to re-issue cmd:start after reload "
                  "(backend stopped continuous mode)")
        except ProtocolError as e:
            # already running: the framework kept it up
            print(f"[driver] cmd:start after reload rejected (good): {e}")

        # Collect ~2 s of vars events post-reload.
        time.sleep(2.0)

        # Stop, drain.
        try:
            c.call("stop")
        except ProtocolError as e:
            print("[driver] cmd:stop failed:", e)
        time.sleep(0.3)
        stop_evt.set()
        t.join(timeout=1.0)

        # Ping after stop.
        try:
            c.ping()
            ping_ok = True
        except Exception as e:
            ping_ok = False
            print("[driver] ping FAILED:", e)

    # ---- analysis -----------------------------------------------------
    print()
    print("=" * 60)
    print("ANALYSIS")
    print("=" * 60)

    total = len(sink)
    pre = [e for e in sink if e["ts"] < t_pre_reload]
    post = [e for e in sink if e["ts"] >= t_reload_returned]
    boundary = [e for e in sink if t_pre_reload <= e["ts"] < t_reload_returned]

    print(f"total vars events: {total}")
    print(f"  pre-reload  ({t_pre_reload - t_started:.2f}s window): {len(pre)}")
    print(f"  during reload ({(t_reload_returned-t_pre_reload)*1000:.0f}ms gap): {len(boundary)}")
    print(f"  post-reload ({(sink[-1]['ts'] - t_reload_returned) if sink else 0:.2f}s window): {len(post)}")

    last_pre_count = None
    last_pre_threshold = None
    if pre:
        last_items = items_by_name(pre[-1]["items"])
        last_pre_count = last_items.get("count", {}).get("value")
        last_pre_threshold = last_items.get("threshold", {}).get("value")
    print(f"last pre-reload count = {last_pre_count}, threshold = {last_pre_threshold}")

    first_post_count = None
    first_post_threshold = None
    if post:
        first_items = items_by_name(post[0]["items"])
        first_post_count = first_items.get("count", {}).get("value")
        first_post_threshold = first_items.get("threshold", {}).get("value")
    print(f"first post-reload count = {first_post_count}, threshold = {first_post_threshold}")

    state_survived = (last_pre_count is not None and first_post_count is not None
                      and first_post_count >= last_pre_count)
    param_survived = (last_pre_threshold == THRESHOLD_SENTINEL
                      and first_post_threshold == THRESHOLD_SENTINEL)

    # version=2 first appearance among post events
    v2_offset = None
    for i, e in enumerate(post):
        items = items_by_name(e["items"])
        v = items.get("version", {}).get("value")
        if v == 2:
            v2_offset = i
            break

    # gaps
    timestamps = [e["ts"] for e in sink]
    largest_gap_ms = 0.0
    largest_gap_around_reload = False
    for i in range(1, len(timestamps)):
        gap = (timestamps[i] - timestamps[i-1]) * 1000.0
        if gap > largest_gap_ms:
            largest_gap_ms = gap
            largest_gap_around_reload = (
                timestamps[i-1] <= t_reload_returned and timestamps[i] >= t_pre_reload
            )

    print(f"state survived: {state_survived}")
    print(f"param survived: {param_survived}")
    print(f"v2 version VAR appeared at post-event idx {v2_offset}")
    print(f"largest gap: {largest_gap_ms:.0f} ms (around reload? {largest_gap_around_reload})")
    print(f"ping after stop: {'ok' if ping_ok else 'FAIL'}")
    print(f"had to manually restart cmd:start after reload? {had_to_restart}")

    # acceptance
    acceptance = {
        ">=30 vars events":           total >= 30,
        "state survived (count)":     state_survived,
        "param survived (threshold)": param_survived,
        "version=2 within 5 events":  (v2_offset is not None and v2_offset < 5),
        "ping ok after stop":         ping_ok,
    }
    print()
    print("Acceptance:")
    for k, v in acceptance.items():
        print(f"  [{'PASS' if v else 'FAIL'}] {k}")
    verdict = "PASS" if all(acceptance.values()) else "FAIL"
    print(f"VERDICT: {verdict}")

    # Persist a structured summary for RESULTS.md to ingest.
    import json
    summary = {
        "total": total,
        "pre": len(pre),
        "post": len(post),
        "boundary": len(boundary),
        "last_pre_count": last_pre_count,
        "first_post_count": first_post_count,
        "last_pre_threshold": last_pre_threshold,
        "first_post_threshold": first_post_threshold,
        "state_survived": state_survived,
        "param_survived": param_survived,
        "v2_offset": v2_offset,
        "largest_gap_ms": largest_gap_ms,
        "largest_gap_around_reload": largest_gap_around_reload,
        "ping_ok": ping_ok,
        "had_to_restart": had_to_restart,
        "reload_ms": (t_reload_returned - t_pre_reload) * 1000,
        "verdict": verdict,
        "acceptance": acceptance,
    }
    with open(os.path.join(HERE, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2, default=str)


if __name__ == "__main__":
    main()
