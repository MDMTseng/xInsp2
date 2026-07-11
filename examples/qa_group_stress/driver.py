"""
Scaled stress: 8 dispatch groups, each with max_parallel 4, each fed a 20/s burst.

8 burst_source instances (one per group g0..g7) each fire 20 triggers/second; each
inspect sleeps a random 150-220ms (avg ~185ms). 4 workers/group give ~21.6/s
capacity vs 20/s arrival — near-saturation, so the queue drains each second on
AVERAGE without piling up. Groups are result_order:"arrival". The driver polls
dispatch_stats throughout and checks, for every group:

  - peak in-flight `running` reaches 4 (all 4 lanes engage on each burst) and
    never exceeds it;
  - queues do not back up (no drops);
  - routing is clean (each lane only ran its own source);
  - run_ids arrive monotonically (0 inversions) under the parallel out-of-order load.

Run:  python examples/qa_group_stress/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()
NGROUPS = 8
MAXP = 4


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_gstress_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = env["TMPDIR"] = str(iso)
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
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})   # trigger-only: no timer, the burst_sources drive it

        peak = defaultdict(int)
        drop_seen = defaultdict(int)
        cap_violation = []
        by_group_src = defaultdict(lambda: defaultdict(int))
        seq_by_group = defaultdict(list)

        def drain_events():
            # Collect run_result events as they STREAM IN (pre-stop). This matters
            # for the ordering check: cmd:stop deliberately releases all in-flight
            # workers out of turn (so stop can't deadlock), which reorders the
            # final ~max_parallel emits per group. Draining during the run captures
            # the steady-state ordered stream and excludes that stop tail.
            while True:
                try: ev = c._inbox_events.get_nowait()
                except Exception: break
                if ev.get("name") == "run_result":
                    d = ev.get("data", {})
                    if d.get("code") == 1:
                        by_group_src[d.get("group")][d.get("msg")] += 1
                        if d.get("run_id") is not None:
                            seq_by_group[d.get("group")].append(d["run_id"])

        end = time.time() + 6.0
        while time.time() < end:
            for g in (c.call("dispatch_stats").get("groups") or []):
                nm, r, mp = g["name"], g.get("running", 0), g.get("max_parallel")
                if r > peak[nm]: peak[nm] = r
                if mp is not None and r > mp: cap_violation.append((nm, r, mp))
                drop_seen[nm] = max(drop_seen[nm], g.get("dropped", 0))
            drain_events()
            time.sleep(0.01)

        c.call("stop")   # steady-state already captured; the post-stop tail is intentionally unordered
        c.call("close_project"); c.close()

        names = [f"g{i}" for i in range(NGROUPS)]
        print(f"{NGROUPS} groups x max_parallel {MAXP}, 20/s burst each:")
        tot = 0
        for g in names:
            got = peak.get(g, 0)
            seq = seq_by_group.get(g, [])
            inv = sum(1 for i in range(len(seq) - 1) if seq[i] > seq[i + 1])
            n = sum(by_group_src.get(g, {}).values())
            tot += n
            print(f"  {g}: peak {got}/{MAXP}  results {n:3d}  drops {drop_seen.get(g,0)}  inversions {inv}")
            if got != MAXP: fails.append(f"{g}: peak running {got} != {MAXP}")
            srcs = set(by_group_src.get(g, {}))
            if srcs and srcs != {f"src_{g}"}: fails.append(f"{g}: foreign source(s) {srcs - {f'src_{g}'}}")
            if not n: fails.append(f"{g}: produced no results")
            if drop_seen.get(g, 0): fails.append(f"{g}: {drop_seen[g]} drops (queue piled up)")
            if inv: fails.append(f"{g}: {inv} run_id inversions")
        if cap_violation: fails.append(f"cap exceeded: {cap_violation[:3]}")
        print(f"  total processed: {tot} across {NGROUPS} groups (~{tot/6:.0f}/s)")
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
