"""cross_proc_trigger — Task #74 validation driver.

Confirms that an isolated source plugin's emit_trigger calls reach
the backend's TriggerBus EVEN WHILE no backend->worker RPC is in
flight. Prior to the always-on reader thread, queued emit_trigger
frames sat in the worker's pipe unread until the backend made a
call into the worker; sources isolated under "isolation":"process"
needed to fall back to "in_process".

Topology
--------
One process-isolated source emitting at 60 Hz; the inspect script
emits one VAR per trigger so the driver can count.

Acceptance window
-----------------
After cmd:start, the driver issues NO further calls to the worker
for 1.0 s and counts inspects. We expect approximately fps * 1.0
events (60); we assert >= 50 to leave headroom for warmup + dispatch
overhead.

Run from this dir:

    python driver.py
"""
from __future__ import annotations

import json
import sys
import time
from collections import Counter
from pathlib import Path
from queue import Empty

from xinsp2 import Client, ProtocolError

ROOT = Path(__file__).parent
INSPECT_CPP = ROOT / "inspect.cpp"

SILENT_WINDOW_S = 1.0
EXPECTED_FPS    = 60
MIN_EVENTS      = 50   # 60 fps minus warmup / dispatch overhead


def drain(c: Client) -> None:
    for q in (c._inbox_vars, c._inbox_previews):
        try:
            while True:
                q.get_nowait()
        except Empty:
            pass


def collect_vars(c: Client, duration_s: float) -> list[dict]:
    events: list[dict] = []
    deadline = time.time() + duration_s
    while time.time() < deadline:
        rem = deadline - time.time()
        try:
            ev = c._inbox_vars.get(timeout=min(0.1, max(0.01, rem)))
        except Empty:
            continue
        flat: dict = {}
        for it in ev.get("items") or []:
            flat[it["name"]] = it.get("value")
        flat["_run_id"] = ev.get("run_id")
        events.append(flat)
    return events


def main() -> int:
    print("cross_proc_trigger — Task #74 validation")
    print(f"  silent window  : {SILENT_WINDOW_S}s")
    print(f"  expected ~     : {EXPECTED_FPS} events")
    print(f"  pass threshold : >= {MIN_EVENTS}")
    print()

    with Client() as c:
        try:
            info = c.open_project(str(ROOT))
        except ProtocolError as e:
            print(f"open_project failed: {e}")
            return 1
        inst_names = [i.get("name") for i in (info.get("instances") or [])]
        print(f"  instances: {inst_names}")
        if "source_iso" not in inst_names:
            print("FAIL: source_iso not in registry — open warnings above?")
            return 1

        c.compile_and_load(str(INSPECT_CPP))
        print("  inspect.cpp compiled")

        # Let the worker's plugin thread spin up before we draw the
        # baseline. The plugin's run_loop_ starts in the constructor;
        # 200 ms is comfortably more than one frame at 60 Hz.
        time.sleep(0.2)
        drain(c)

        # cmd:start — backend dispatch pool comes up, TriggerBus sink
        # forwards events into the queue. After this point we make NO
        # backend->worker RPCs until the silent window closes.
        c.call("start", {"fps": 1})  # backend timer is irrelevant here;
                                     # source emits autonomously at 60 Hz
        print(f"  silent window starting (no backend->worker RPCs)")
        t0 = time.time()
        events = collect_vars(c, SILENT_WINDOW_S)
        elapsed = time.time() - t0
        c.call("stop")

        active = [e for e in events if e.get("active") is True]
        n_total  = len(events)
        n_active = len(active)
        by_src = Counter(e.get("src") or "?" for e in active)

        seqs = sorted(int(e["seq"]) for e in active
                      if isinstance(e.get("seq"), (int, float)))
        seq_min = seqs[0]  if seqs else None
        seq_max = seqs[-1] if seqs else None
        seq_uniq = len(set(seqs))

        print()
        print(f"  elapsed                : {elapsed:.3f}s")
        print(f"  total vars events      : {n_total}")
        print(f"  active inspects        : {n_active}")
        print(f"  by source              : {dict(by_src)}")
        print(f"  seq min / max / unique : {seq_min} / {seq_max} / {seq_uniq}")
        if elapsed > 0:
            print(f"  effective trigger rate : "
                  f"{n_active / elapsed:.1f} Hz")

        ok = n_active >= MIN_EVENTS
        if ok:
            print()
            print(f"PASS: {n_active} >= {MIN_EVENTS} during silent window")
            print("      (cross-process emit_trigger reader is online)")
            # Useful for log scraping in CI.
            (ROOT / "driver_summary.json").write_text(
                json.dumps({"active": n_active, "elapsed": elapsed,
                            "rate_hz": n_active / elapsed if elapsed else 0.0,
                            "by_src": dict(by_src),
                            "result": "PASS"}, indent=2) + "\n")
            return 0

        print()
        print(f"FAIL: only {n_active} active inspects in {elapsed:.3f}s "
              f"(threshold {MIN_EVENTS})")
        print("      Expected ~60; if the reader thread is offline this "
              "stays at 0–1.")
        (ROOT / "driver_summary.json").write_text(
            json.dumps({"active": n_active, "elapsed": elapsed,
                        "by_src": dict(by_src), "result": "FAIL"},
                       indent=2) + "\n")
        return 2


if __name__ == "__main__":
    sys.exit(main())
