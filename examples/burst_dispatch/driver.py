"""Verify dispatch_threads=4 parallelises slow inspects.

Each inspect sleeps 50 ms. fps=100 -> timer pushes 100 events/sec into
the queue. With 4 workers each handling 20 events/sec (50ms each), the
ceiling is ~80 events/sec. Anything close to that confirms parallelism;
single-threaded would cap at 20 events/sec.

Throughput is measured by counting the per-inspect `run_finished`
lifecycle events the backend emits into the client's event inbox
(`_inbox_events`). This pure-observability count replaces the old
`next_vars()` path (the Python SDK is now generic and no longer
collects script VARs) — the number of completed runs in the window
IS the throughput we want to measure.
"""
import time
from pathlib import Path
from queue import Empty

from xinsp2 import Client

ROOT = Path(__file__).parent
SLEEP_MS = 50


def drain_run_finished(c: Client) -> int:
    """Pop every queued event; return how many were `run_finished`."""
    n = 0
    while True:
        try:
            ev = c._inbox_events.get_nowait()
        except Empty:
            break
        if ev.get("name") == "run_finished":
            n += 1
    return n


with Client() as c:
    proj = c.open_project(str(ROOT), timeout=240)
    print("project:", proj.get("name"))
    c.compile_and_load(str(ROOT / "inspect.cpp"))

    # Discard any events that landed during open/compile so the window
    # count is clean.
    drain_run_finished(c)

    rsp = c.call("start", {"fps": 100})
    print("start rsp:", rsp)

    deadline = time.time() + 1.0
    runs = 0
    while time.time() < deadline:
        runs += drain_run_finished(c)
        time.sleep(0.01)

    c.call("stop")
    # Scoop any in-flight completions that landed after the window closed.
    runs += drain_run_finished(c)
    elapsed_s = 1.0
    print(f"run_finished events received: {runs}")

    expected_serial    = elapsed_s * 1000 / SLEEP_MS              # ~20
    expected_parallel4 = elapsed_s * 1000 / SLEEP_MS * 4          # ~80
    print(f"expected serial  ~= {expected_serial:.0f}")
    print(f"expected 4x par. ~= {expected_parallel4:.0f}")
    threshold = expected_serial * 2.5
    print(f"threshold (2.5x serial): {threshold:.0f}")
    if runs >= threshold:
        print("PASS - parallel dispatch is working")
    else:
        print("FAIL - looks serial")
