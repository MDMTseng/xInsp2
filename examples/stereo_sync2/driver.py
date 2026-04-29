"""driver.py — stereo_sync2 verification.

Connects to the xInsp2 backend, opens this project, compiles inspect.cpp,
turns on continuous mode, then observes ~5 s of `vars` messages to
confirm the framework correlates emits from cam_left and cam_right by
trigger id.

Acceptance:
- >=30 cycles observed
- >=95% matched
- backend stays up for the full 5 s
- cycle count is roughly proportional (20 Hz * 5 s = 100 +/- 20%)
"""
from __future__ import annotations

import os
import sys
import time
from queue import Empty

# Make the SDK importable when running directly: python driver.py
_THIS = os.path.dirname(os.path.abspath(__file__))
_SDK  = os.path.normpath(os.path.join(_THIS, "..", "..", "tools", "xinsp2_py"))
if _SDK not in sys.path:
    sys.path.insert(0, _SDK)

from xinsp2.client import Client, ProtocolError  # noqa: E402


PROJECT_DIR = _THIS.replace("\\", "/")
SCRIPT_PATH = os.path.join(PROJECT_DIR, "inspect.cpp").replace("\\", "/")
SOAK_SECONDS = 5.0


def _vars_to_dict(items):
    return {it["name"]: it.get("value") for it in items}


def main():
    print(f"[driver] project = {PROJECT_DIR}")
    with Client() as c:
        # 1. Open the project — backend compiles synced_cam, instantiates
        #    cam_left + cam_right, and (because their config has
        #    auto_start=true) their worker threads start emitting.
        print("[driver] open_project ...")
        op = c.open_project(PROJECT_DIR)
        print(f"[driver]   plugins   : {[p['name'] for p in op.get('plugins', [])]}")
        print(f"[driver]   instances : {[i.get('name') for i in op.get('instances', [])]}")

        # 2. Compile + load the script.
        print("[driver] compile_and_load ...")
        c.compile_and_load(SCRIPT_PATH)

        # 3. Drain stale messages so we only count the soak window.
        for q in (c._inbox_vars, c._inbox_previews):
            while True:
                try: q.get_nowait()
                except Empty: break

        # 4. Start continuous mode. fps is the timer-fallback rate; with
        #    the trigger bus active it just upper-bounds the wait, so a
        #    higher value lets us not miss bus events.
        print("[driver] start (continuous) ...")
        c.call("start", {"fps": 30})

        # 5. Observe vars for SOAK_SECONDS.
        cycles = []
        deadline = time.monotonic() + SOAK_SECONDS
        while time.monotonic() < deadline:
            try:
                msg = c._inbox_vars.get(timeout=0.2)
            except Empty:
                continue
            d = _vars_to_dict(msg.get("items", []))
            cycles.append(d)

        # 6. Stop.
        print("[driver] stop ...")
        c.call("stop")

        # 7. Sanity-check backend is still alive.
        try:
            c.ping()
            backend_alive = True
        except Exception as e:
            backend_alive = False
            print(f"[driver] ping after stop failed: {e}")

        # ---------- analysis ----------
        # The continuous-mode worker fires two kinds of cycles:
        #   - bus-driven (active=True)   : a TriggerEvent reached the script.
        #   - timer fallback (active=False) : nothing on the bus yet, the
        #     worker tick-fired anyway (back-compat with non-source pipelines).
        # We only care about trigger-driven cycles for this test. The total
        # count is reported for transparency but acceptance is on the bus
        # cycles.
        n_total = len(cycles)
        bus_cycles = [d for d in cycles if d.get("active") is True]
        n_bus = len(bus_cycles)
        matched = sum(1 for d in bus_cycles if d.get("matched") is True)
        have_l  = sum(1 for d in bus_cycles if d.get("have_left")  is True)
        have_r  = sum(1 for d in bus_cycles if d.get("have_right") is True)

        samples = []
        for idx in (0, n_bus // 2, n_bus - 1) if n_bus else ():
            d = bus_cycles[idx]
            samples.append({k: d.get(k) for k in
                            ("active", "have_left", "have_right",
                             "left_seq", "right_seq", "matched", "tid")})

        print(f"\n[result] cycles observed (total)  : {n_total}")
        print(f"[result] cycles bus-driven        : {n_bus}")
        print(f"[result] have_left/right=True     : {have_l} / {have_r}")
        print(f"[result] matched (left_seq==right_seq): {matched}")
        if n_bus:
            print(f"[result] match rate               : {matched / n_bus * 100:.1f}%")
        print(f"[result] backend alive after stop : {backend_alive}")
        for i, s in enumerate(samples):
            print(f"[result] sample[{i}] {s}")

        # Acceptance: 20 Hz * 5 s ~= 100 bus cycles +/- 20%.
        cycle_count_ok = 80 <= n_bus <= 120
        ok = (
            n_bus >= 30 and
            matched / n_bus >= 0.95 and
            backend_alive and
            cycle_count_ok
        )
        print(f"\n[result] VERDICT: {'PASS' if ok else 'FAIL'}")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
