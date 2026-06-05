"""
Per-group max_parallel under a 1Hz burst.

Three groups p1/p2/p4 (max_parallel 1/2/4). A burst_source per group fires 3x the
group's worker count every second (instantaneous surge), and each inspect sleeps a
random 50-100ms so the work overlaps. The driver polls dispatch_stats `running`
(in-flight inspects per lane) throughout and checks:

  - each group's PEAK `running` reaches its max_parallel (the lanes really do run
    in parallel: p1->1, p2->2, p4->4);
  - `running` NEVER exceeds max_parallel (the cap holds);
  - routing is clean (each lane only ran its own source) and every group made
    progress.

Run:  python examples/qa_group_parallelism/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7899"))
EXPECT = {"p1": 1, "p2": 2, "p4": 4}


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_gpar_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port):
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60); c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})       # compiles burst_source too
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 2})

        # Poll the live per-lane `running` count for ~6s (covers several 1Hz bursts).
        peak = defaultdict(int)
        cap_violation = []
        end = time.time() + 6.0
        while time.time() < end:
            for g in (c.call("dispatch_stats").get("groups") or []):
                nm, r, mp = g["name"], g.get("running", 0), g.get("max_parallel")
                if r > peak[nm]: peak[nm] = r
                if mp is not None and r > mp: cap_violation.append((nm, r, mp))
            time.sleep(0.01)

        c.call("stop")
        # Drain run_result events for per-group throughput + routing.
        by_group_src = defaultdict(lambda: defaultdict(int))
        while True:
            try: ev = c._inbox_events.get_nowait()
            except Exception: break
            if ev.get("name") == "run_result":
                d = ev.get("data", {})
                if d.get("code") == 1:
                    by_group_src[d.get("group")][d.get("msg")] += 1
        c.call("close_project"); c.close()

        print("peak running per group:", {k: peak.get(k, 0) for k in EXPECT})
        for g, want in EXPECT.items():
            got = peak.get(g, 0)
            print(f"  {g}: peak {got} / max_parallel {want}, "
                  f"results {dict(by_group_src.get(g, {}))}")
            if got != want:
                fails.append(f"{g}: peak running {got} != max_parallel {want}")
            srcs = set(by_group_src.get(g, {}))
            expect_src = {"p1": "src_p1", "p2": "src_p2", "p4": "src_p4"}[g]
            if srcs and srcs != {expect_src}:
                fails.append(f"{g}: foreign source(s) {srcs - {expect_src}}")
            if not by_group_src.get(g):
                fails.append(f"{g}: produced no results")
        if cap_violation:
            fails.append(f"cap exceeded: {cap_violation[:3]}")
    except Exception as e:
        fails.append(f"{e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
