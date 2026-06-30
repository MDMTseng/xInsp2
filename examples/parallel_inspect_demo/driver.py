"""parallel_inspect_demo — multi-camera trigger x slow inspect.

Question: if process() is slow (say 100 ms) AND multiple triggers
arrive close together (e.g. hardware-synchronised cameras), can the
inspect calls run in parallel?

Answer: yes — `parallelism.dispatch_threads` in `project.json`
controls how many dispatcher worker threads pull events off the
trigger queue. With dispatch_threads=N, up to N inspects run
concurrently.

Topology
--------
- 3 burst_source instances ("cam_left", "cam_center", "cam_right"),
  shape:steady at 30 fps each — total 90 events/sec offered to the
  dispatcher.
- inspect.cpp does sleep_for(100 ms) — stand-in for a real CV stage.
- Run for COLLECT_S seconds, count active inspects.

If dispatch_threads=1, the pipeline tops out at ~10 inspects/sec
(serialised on a 100 ms stage). With dispatch_threads=3, three
inspects run in parallel and we expect ~30 inspects/sec.

Throughput is measured by counting *active* inspects (the slow 100 ms
ones — the metric this demo is about). The per-event VAR model was
removed from core, so each active inspect pushes a tiny record to the
`expose` sink on channel "runs"; the driver subscribes and counts the
decoded XEX1 frames. (Counting raw `run_finished` events would also
sweep in the cheap inactive timer ticks that fire between source events,
diluting the rate this demo is meant to show.)

Run from this dir:

    python driver.py
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path

from xinsp2 import Client

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from xex1 import collect_frames, subscribe   # noqa: E402

ROOT = Path(__file__).parent
INSPECT_CPP = ROOT / "inspect.cpp"
PROJECT_JSON = ROOT / "project.json"
BACKEND_EXE = (ROOT.parents[1] / "backend" / "build" /
               "Release" / "xinsp-backend.exe")
COLLECT_S = 4.0
INSPECT_MS = 100   # mirrors the sleep in inspect.cpp; expected per-call work


def set_dispatch_threads(n: int) -> None:
    cfg = json.loads(PROJECT_JSON.read_text())
    cfg["parallelism"]["dispatch_threads"] = n
    PROJECT_JSON.write_text(json.dumps(cfg, indent=2) + "\n")


def drain(c: Client) -> None:
    c.drain_binary()


def measure(label: str, n_threads: int) -> dict:
    print(f"\n=== {label} (dispatch_threads={n_threads}) ===")
    set_dispatch_threads(n_threads)

    proc = subprocess.Popen([str(BACKEND_EXE)],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    time.sleep(0.4)

    try:
        with Client() as c:
            c.open_project(str(ROOT), timeout=180)
            c.compile_and_load(str(INSPECT_CPP))
            # Subscribe so the expose sink pushes a frame per active
            # inspect (subscription is reset by open_project).
            subscribe(c, ["runs"])
            time.sleep(0.3)
            drain(c)

            c.call("start", {"fps": 100})
            time.sleep(0.5)
            # Clear warmup frames so the window count only reflects this
            # measurement.
            drain(c)

            t0 = time.monotonic()
            # Count active inspects via the expose "runs" frames — one per
            # 100 ms inspect; that IS the effective rate.
            n = 0
            deadline = t0 + COLLECT_S
            while time.monotonic() < deadline:
                for fr in collect_frames(c):
                    if fr.get("channel") == "runs":
                        n += 1
                time.sleep(0.02)

            c.call("stop")
            # Catch any trailing frames before tearing down.
            for fr in collect_frames(c):
                if fr.get("channel") == "runs":
                    n += 1
            c.call("close_project")

            elapsed = time.monotonic() - t0
            rate = n / elapsed if elapsed > 0 else 0
            print(f"  active inspects   : {n}  (per-call work ~{INSPECT_MS} ms)")
            print(f"  effective rate    : {rate:.1f}/s")
            return {"n_threads": n_threads, "elapsed_s": elapsed,
                    "active": n, "rate_hz": rate}
    finally:
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill()


def main() -> int:
    print("parallel_inspect_demo — multi-camera trigger x slow inspect")
    print(f"  3 sources @ 30 fps each (90 events/s offered)")
    print(f"  inspect = sleep_for {INSPECT_MS} ms")
    print(f"  collect window: {COLLECT_S}s per mode")
    if not BACKEND_EXE.exists():
        print(f"FAIL: backend exe not found: {BACKEND_EXE}")
        return 1

    serial   = measure("SERIAL  ", 1)
    parallel = measure("PARALLEL", 3)

    set_dispatch_threads(3)  # restore in-PR default

    speedup = (parallel["rate_hz"] / serial["rate_hz"]
               if serial["rate_hz"] else None)

    print()
    print("=" * 64)
    print(f"{'metric':<24}{'serial (N=1)':>18}{'parallel (N=3)':>18}")
    print("-" * 64)
    print(f"{'active inspects':<24}{serial['active']:>18d}{parallel['active']:>18d}")
    print(f"{'rate (inspects/s)':<24}"
          f"{serial['rate_hz']:>15.1f}/s{parallel['rate_hz']:>15.1f}/s")
    print("=" * 64)
    if speedup:
        print(f"\nspeedup (parallel / serial rate):  {speedup:.2f}x")
        print(f"theoretical max with N=3 threads:  3.00x")
        print(f"\nserial:   ~10/s expected (1/{INSPECT_MS}ms)")
        print(f"parallel: ~30/s expected (3 threads x 10/s each)")

    (ROOT / "results.json").write_text(
        json.dumps({"serial": serial, "parallel": parallel,
                    "speedup": speedup,
                    "inspect_ms_each": INSPECT_MS,
                    "collect_s": COLLECT_S}, indent=2) + "\n")
    print(f"\nwrote {ROOT / 'results.json'}")

    return 0 if speedup and speedup >= 2.0 else 1


if __name__ == "__main__":
    sys.exit(main())
