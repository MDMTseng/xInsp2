"""Verify queue overflow policy actually drops events under sustained load.

Setup:
  - dispatch_threads=1, queue_depth=4, drop_oldest.
  - sleep_ms=50 per inspect -> ~20 events/sec capacity.
  - fps=200 nominally; Windows timer granularity caps real rate ~67/sec
    (Sleep tick is ~15 ms unless timeBeginPeriod). So timer pushes ~67/sec
    and ~20/sec get processed; the rest (~47/sec) get dropped.

Expected:
  - dropped_oldest grows by ~30-50 per second.
  - queue_depth_now stays at the cap (4) under load.
  - Backend stays alive; memory bounded.
"""
import json
import time
from pathlib import Path

from xinsp2 import Client

ROOT = Path(__file__).parent

# Patch project.json for this run.
proj_path = ROOT / "project.json"
saved = proj_path.read_text()
proj = json.loads(saved)
proj["parallelism"] = {"dispatch_threads": 1, "queue_depth": 4, "overflow": "drop_oldest"}
proj_path.write_text(json.dumps(proj, indent=2))

try:
    with Client() as c:
        c.open_project(str(ROOT), timeout=240)
        c.compile_and_load(str(ROOT / "inspect.cpp"))

        before = c.call("dispatch_stats")
        print("before start:", before)

        c.call("start", {"fps": 200})
        time.sleep(1.0)

        mid = c.call("dispatch_stats")
        print("after 1s:", mid)

        c.call("stop")
        after = c.call("dispatch_stats")
        print("after stop:", after)

        dropped = mid["dropped_oldest"]
        qsz = mid["queue_depth_now"]
        if dropped > 20 and qsz == 4:
            print(f"PASS - {dropped} drops in 1s, queue stayed pinned at cap (4)")
        else:
            print(f"FAIL - drops={dropped} qsz={qsz} (expected drops>20 qsz=4)")
finally:
    proj_path.write_text(saved)
