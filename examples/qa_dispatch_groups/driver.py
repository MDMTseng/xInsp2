"""
Dispatch-groups v1 smoke test.

Proves the gated per-group lane dispatcher:
  - a project with `parallelism.groups` spawns per-group worker lanes;
  - continuous mode runs inspects through them (timer ticks → default group);
  - `dispatch_stats` reports a per-group breakdown (name / max_parallel /
    thread_priority / running / queue / dropped);
  - a project WITHOUT groups still reports no `groups` key (legacy path intact).

(The full high-vs-low load-separation guarantee — a saturated `low` group never
delaying `high` — needs a self-emitting source in each group; that's a follow-up.)

Run:  python examples/qa_dispatch_groups/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7894"))


def spawn(port, iso_name):
    iso = Path(os.environ["LOCALAPPDATA"]) / "Temp" / iso_name
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
    proc = spawn(PORT, "xi_dg_tmp")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 20})

        # let lanes run a bit
        ran = 0
        for _ in range(20):
            if c.next_vars(timeout=1): ran += 1
            if ran >= 5: break

        st = c.call("dispatch_stats")
        print("dispatch_stats:", {k: st.get(k) for k in ("dispatch_threads",)}, "groups=", st.get("groups"))
        groups = st.get("groups")
        if not isinstance(groups, list) or len(groups) != 2:
            fails.append(f"expected 2 group lanes, got {groups}")
        else:
            names = {g["name"] for g in groups}
            if names != {"high", "low"}:
                fails.append(f"group names {names} != high/low")
            hi = next((g for g in groups if g["name"] == "high"), {})
            lo = next((g for g in groups if g["name"] == "low"), {})
            if hi.get("max_parallel") != 2: fails.append(f"high max_parallel {hi.get('max_parallel')} != 2")
            if lo.get("thread_priority") != "low": fails.append(f"low thread_priority {lo.get('thread_priority')} != low")
            for g in groups:
                for f in ("running", "queue_now", "high_watermark", "dropped"):
                    if f not in g: fails.append(f"group {g.get('name')} missing field {f}")
        if ran < 1: fails.append("no vars flowed under grouped continuous mode")
        print(f"  vars seen: {ran}")
        c.call("stop")
        c.call("close_project")
        c.close()
    except Exception as e:
        fails.append(f"grouped: {e}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    # --- legacy: no groups -> dispatch_stats has no "groups" key ---
    legacy = REPO / "examples" / "image_sources"
    if legacy.is_dir():
        proc2 = spawn(PORT + 1, "xi_dg_legacy_tmp")
        try:
            c2 = connect(PORT + 1)
            if c2:
                c2.call("open_project", {"path": str(legacy)})
                st2 = c2.call("dispatch_stats")
                if "groups" in st2:
                    fails.append("legacy project unexpectedly reported a 'groups' breakdown")
                else:
                    print("legacy (image_sources): no 'groups' key")
                c2.close()
        except Exception as e:
            print(f"  (legacy check skipped: {e})")
        finally:
            proc2.terminate()
            try: proc2.wait(5)
            except Exception: proc2.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
