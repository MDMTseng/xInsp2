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

Run from this dir:

    python driver.py
"""
from __future__ import annotations

import json
import statistics
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path
from queue import Empty

from xinsp2 import Client

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
    for q in (c._inbox_vars, c._inbox_previews):
        try:
            while True: q.get_nowait()
        except Empty: pass


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
            time.sleep(0.3)
            drain(c)

            c.call("start", {"fps": 100})
            time.sleep(0.5)
            drain(c)

            t0 = time.monotonic()
            arrivals: list[float] = []
            inspect_us_samples: list[float] = []
            by_src: Counter = Counter()
            deadline = t0 + COLLECT_S
            while time.monotonic() < deadline:
                rem = deadline - time.monotonic()
                try:
                    ev = c._inbox_vars.get(timeout=min(0.05, max(0.005, rem)))
                except Empty:
                    continue
                items = {it["name"]: it.get("value")
                         for it in (ev.get("items") or [])}
                if items.get("active") is True:
                    arrivals.append(time.monotonic())
                    src = items.get("src")
                    if src: by_src[src] += 1
                    iu = items.get("inspect_us")
                    if isinstance(iu, (int, float)): inspect_us_samples.append(iu)

            c.call("stop")
            c.call("close_project")

            elapsed = time.monotonic() - t0
            n = len(arrivals)
            rate = n / elapsed if elapsed > 0 else 0
            print(f"  active inspects   : {n}")
            print(f"  effective rate    : {rate:.1f}/s")
            print(f"  by_src            : {dict(by_src)}")
            if inspect_us_samples:
                im = statistics.mean(inspect_us_samples) / 1000
                ip = sorted(inspect_us_samples)[
                         int(len(inspect_us_samples) * 0.95)] / 1000
                print(f"  inspect_us mean   : {im:.1f} ms (~{INSPECT_MS} expected)")
                print(f"  inspect_us p95    : {ip:.1f} ms")
            return {"n_threads": n_threads, "elapsed_s": elapsed,
                    "active": n, "rate_hz": rate, "by_src": dict(by_src)}
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
