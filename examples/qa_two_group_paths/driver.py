"""
Two dispatch groups, two independent paths.

Two frame_source instances self-emit triggers tagged (via instance.json "group")
into different group lanes: src_fast -> group "fast" (40fps, 3 threads), src_slow
-> group "slow" (8fps, 1 thread). The dispatcher routes each by the EMITTING
instance's group, so the streams never share a lane.

Asserts:
  - dispatch_stats reports both lanes (fast/slow);
  - both groups produce real (code==1) run_result events — two live paths;
  - routing is correct: every "fast"-group result came from src_fast and every
    "slow"-group result from src_slow (the group tag matches the source);
  - the fast path runs at its own (higher) cadence — fast count >> slow count —
    i.e. the slow group does not gate the fast one.

Run:  python examples/qa_two_group_paths/driver.py   (Windows; backend built)
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
PORT = int(os.environ.get("PORT", "7898"))


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_2grp_iso"
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
        c.call("open_project", {"path": str(ROOT)})       # compiles frame_source too
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 2})                        # low timer; the sources self-drive

        # Collect run_result events for a few seconds, bucketed by group.
        by_group_src = defaultdict(lambda: defaultdict(int))   # group -> source(msg) -> count
        end = time.time() + 3.0
        while time.time() < end:
            try:
                ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception:
                break
            if ev.get("name") != "run_result":
                continue
            d = ev.get("data", {})
            if d.get("code") != 1:          # ignore timer-tick NAs (code 0) + any drops
                continue
            by_group_src[d.get("group")][d.get("msg")] += 1

        st = c.call("dispatch_stats")
        lanes = {g["name"] for g in (st.get("groups") or [])}
        print("lanes:", sorted(lanes))
        if lanes != {"fast", "slow"}:
            fails.append(f"expected lanes fast/slow, got {sorted(lanes)}")

        fast = dict(by_group_src.get("fast", {}))
        slow = dict(by_group_src.get("slow", {}))
        nf, ns = sum(fast.values()), sum(slow.values())
        print(f"fast group: {nf} results {fast}")
        print(f"slow group: {ns} results {slow}")

        if nf < 20: fails.append(f"fast group produced too few results ({nf})")
        if ns < 5:  fails.append(f"slow group produced too few results ({ns})")
        # Routing: each group only ever carried its own source.
        if set(fast) - {"src_fast"}: fails.append(f"fast lane carried foreign source(s): {set(fast) - {'src_fast'}}")
        if set(slow) - {"src_slow"}: fails.append(f"slow lane carried foreign source(s): {set(slow) - {'src_slow'}}")
        # Independent cadence: the 40fps path clearly outruns the 8fps one.
        if not (nf > ns): fails.append(f"fast ({nf}) did not outrun slow ({ns}) — paths not independent?")

        c.call("stop"); c.call("close_project"); c.close()
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
