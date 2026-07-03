"""
Per-group CPU affinity (multi-core mask).

Two groups: "bound" workers have cpu_affinity [2,3,4,5] (each worker may run on ANY
of those four cores — a multi-core mask, not pinned to one); "free" is unbound. Two
burst_sources feed them. Each inspect records GetCurrentProcessorNumber. Asserts:

  - dispatch_stats reports the configured affinity for "bound";
  - every "bound" run executed on a core in {2,3,4,5} (binding took effect), and it
    used MORE THAN ONE of them (multi-core, not a single pin);
  - "free" ran and was NOT confined to {2,3,4,5} (spread across other cores) — a
    sanity check that the affinity is per-group, not global.

Needs a machine with enough cores; SKIPs if < 8. Windows-only (GetCurrentProcessorNumber).

Run:  python examples/qa_cpu_affinity/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "0")) or free_port()
BOUND_SET = {2, 3, 4, 5}


def spawn(port):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "xi_aff_iso"
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
        print("SKIP: Windows-only (GetCurrentProcessorNumber)"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    if (os.cpu_count() or 0) < 8:
        print(f"SKIP: needs >= 8 cores (have {os.cpu_count()})"); return 0
    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 0})   # trigger-only

        cores_by_group = defaultdict(list)
        end = time.time() + 4.0
        while time.time() < end:
            try: ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
            except Exception: break
            if ev.get("name") == "run_result":
                d = ev.get("data", {})
                if d.get("code") == 1:
                    try: cores_by_group[d.get("group")].append(int(d.get("msg")))
                    except (TypeError, ValueError): pass

        st = {g["name"]: g for g in (c.call("dispatch_stats").get("groups") or [])}
        c.call("stop"); c.call("close_project"); c.close()

        aff = st.get("bound", {}).get("cpu_affinity")
        print(f"dispatch_stats bound.cpu_affinity = {aff}")
        if aff != [[2, 3, 4, 5]]:
            fails.append(f"bound cpu_affinity not reported as [[2,3,4,5]] (got {aff})")
        if st.get("free", {}).get("cpu_affinity") != []:
            fails.append("free group should report empty cpu_affinity")

        bound = cores_by_group.get("bound", [])
        free = cores_by_group.get("free", [])
        bset, fset = set(bound), set(free)
        print(f"bound group ran on cores {sorted(bset)} ({len(bound)} runs)")
        print(f"free  group ran on cores {sorted(fset)} ({len(free)} runs)")
        if len(bound) < 5:
            fails.append(f"too few bound runs ({len(bound)}) to judge")
        if not bset.issubset(BOUND_SET):
            fails.append(f"bound group ran OUTSIDE its affinity: {sorted(bset - BOUND_SET)}")
        if len(bset) < 2:
            fails.append(f"bound group used only {bset} — expected multiple cores (multi-core mask)")
        if free and fset.issubset(BOUND_SET):
            fails.append(f"free group stayed within {BOUND_SET} — affinity leaked / not per-group?")
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
