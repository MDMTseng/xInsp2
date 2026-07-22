"""
qa_remove_under_load — Finding-5 stress regression (red-team load pass,
docs/new_gen/21-redteam-load-findings.md).

remove_instance was the ONLY DLL-affecting lifecycle op with no
quiesce_dispatch_for_lifecycle_op_ guard (service_dispatch.cpp), so removing an
instance whose owner-tagged pack refs are in flight on a dispatch worker could
leave a phantom ledger bucket the sweep later cashes — a UAF window. The fix
wraps cmd_remove_instance_ in the same quiesce guard every sibling op uses.

This driver drives mock_camera (PACK MODE) as a continuous stream while the
inspect script chains each trigger pack into the churned "victim" instance's
pack door (so its refs ride the dispatch flow), and HAMMERS remove_instance /
create_instance on the victim under that load.

Asserts (survival + consistency, not per-frame timing):
  1. Every remove_instance / create_instance under load returns ok (no
     ProtocolError, no dropped connection).
  2. The backend stays alive + responsive (ping ok) through every cycle and
     after — a crash from the racing worker would drop the socket.
  3. Frames keep flowing (>0 stream frames observed), i.e. the pool resumes
     after each quiesce (the guard is symmetric).
  4. Final instance set is consistent (victim present, cam + expose intact).

Run:  python qa/qa_remove_under_load/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import shutil, tempfile, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402
from xex1 import collect_frames, subscribe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()
CYCLES = 15


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_remove_under_load_iso"
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
    frames_seen = 0
    # This test MUTATES its own project by design — it creates a "victim"
    # instance and churns it under load — and the backend persists that into
    # project.json on the way out. Left alone, every run leaves the fixture
    # dirty in git and the next reader cannot tell test residue from a real
    # edit. Snapshot the bytes now, put them back in finally.
    proj = ROOT / "project.json"
    proj_orig = proj.read_bytes()
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        subscribe(c, ["rul"])

        # The victim: a real consumer the script chains into every frame.
        c.call("create_instance", {"name": "victim", "plugin": "blob_analysis"})

        c.call("start", {"fps": 0})
        c.drain_binary()
        c.exchange_instance("cam", {"command": "start"})
        time.sleep(0.4)   # let the stream spin up so removes land under real load

        for i in range(CYCLES):
            try:
                c.call("remove_instance", {"name": "victim"})       # under live load
                time.sleep(0.05)
                c.call("create_instance", {"name": "victim", "plugin": "blob_analysis"})
                c.ping()                                            # backend still alive?
            except Exception as e:
                fails.append(f"cycle {i}: {e!r}")
                break
            for fr in collect_frames(c):
                if fr.get("channel") == "rul":
                    frames_seen += 1

        # Drain a bit more traffic to confirm the pool resumed after the last quiesce.
        end = time.time() + 1.5
        while time.time() < end:
            for fr in collect_frames(c):
                if fr.get("channel") == "rul":
                    frames_seen += 1
            time.sleep(0.1)

        c.exchange_instance("cam", {"command": "stop"})
        c.call("stop")

        # Consistency: the backend answers, and the instance set is intact.
        c.ping()
        insts = c.list_instances()
        names = {i.get("name") for i in (insts.get("instances") or [])} if isinstance(insts, dict) else set()
        for want in ("cam", "expose", "victim"):
            if want not in names:
                fails.append(f"instance '{want}' missing after churn (got {sorted(names)})")

        if frames_seen == 0:
            fails.append("no stream frames observed — dispatch never resumed after a quiesce")

        print(f"cycles={CYCLES} frames_seen={frames_seen} instances={sorted(names)}")

    except Exception as e:
        fails.append(f"exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()
        # Restore the fixture (see the snapshot above). Do it after the backend
        # is down, or it will just persist its in-memory copy over the top.
        proj.write_bytes(proj_orig)
        shutil.rmtree(ROOT / "instances" / "victim", ignore_errors=True)

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: remove_instance-under-load")
        return 1
    print("VERDICT: PASS: remove_instance quiesces under continuous dispatch load - "
          "no crash, pool resumes, instance set consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
