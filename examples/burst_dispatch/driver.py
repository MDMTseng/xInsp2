"""Verify dispatch_threads=4 parallelises slow inspects.

Each inspect sleeps 50 ms. fps=100 -> timer pushes 100 events/sec into
the queue. With 4 workers each handling 20 events/sec (50ms each), the
ceiling is ~80 events/sec. Anything close to that confirms parallelism;
single-threaded would cap at 20 events/sec.
"""
import time
from pathlib import Path

from xinsp2 import Client

ROOT = Path(__file__).parent
SLEEP_MS = 50

with Client() as c:
    proj = c.open_project(str(ROOT), timeout=240)
    print("project:", proj.get("name"))
    c.compile_and_load(str(ROOT / "inspect.cpp"))

    rsp = c.call("start", {"fps": 100})
    print("start rsp:", rsp)

    deadline = time.time() + 1.0
    count_seen = 0
    max_count = 0
    while time.time() < deadline:
        v = c.next_vars(timeout=0.05)
        if v is None: continue
        count_seen += 1
        for item in v.get("items", []):
            if item.get("name") == "count":
                max_count = max(max_count, item.get("value", 0))

    c.call("stop")
    elapsed_s = 1.0
    print(f"vars events received: {count_seen}, max count seen: {max_count}")

    expected_serial    = elapsed_s * 1000 / SLEEP_MS              # ~20
    expected_parallel4 = elapsed_s * 1000 / SLEEP_MS * 4          # ~80
    print(f"expected serial  ~= {expected_serial:.0f}")
    print(f"expected 4x par. ~= {expected_parallel4:.0f}")
    threshold = expected_serial * 2.5
    print(f"threshold (2.5x serial): {threshold:.0f}")
    if max_count >= threshold:
        print("PASS - parallel dispatch is working")
    else:
        print("FAIL - looks serial")
