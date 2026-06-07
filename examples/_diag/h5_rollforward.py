"""H5 PoC — an interrupted working-copy commit is rolled forward (not left torn).

Simulate a crash mid-commit:
  - canonical project + a .xinsp_work scratch that contains an extra file
    (SENTINEL.txt) representing an edit that was being committed,
  - a leftover .xinsp_commit_pending marker (commit started, never finished),
  - the canonical does NOT yet have SENTINEL.txt (torn / partial commit).
Then open the project in working-copy mode. open_project must detect the marker
+ intact scratch and re-run the (idempotent) mirror, healing the canonical.

Verdict: canonical gains SENTINEL.txt AND the marker is gone -> H5 FIXED.
"""
from __future__ import annotations
import shutil, sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "h5_proj"
SCRATCH = PROJ / ".xinsp_work"
MARKER = PROJ / ".xinsp_commit_pending"
SENTINEL_REL = "SENTINEL.txt"
PORT = 7907


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1

    # ---- arrange the interrupted-commit state ----
    if SCRATCH.exists(): shutil.rmtree(SCRATCH)
    if MARKER.exists(): MARKER.unlink()
    canonical_sentinel = PROJ / SENTINEL_REL
    if canonical_sentinel.exists(): canonical_sentinel.unlink()

    # scratch = a complete copy of the project + the in-flight edit (SENTINEL)
    SCRATCH.mkdir(parents=True)
    shutil.copy(PROJ / "project.json", SCRATCH / "project.json")
    shutil.copy(PROJ / "inspect.cpp", SCRATCH / "inspect.cpp")
    (SCRATCH / SENTINEL_REL).write_text("committed edit that the crash interrupted\n", encoding="utf-8")
    # commit-in-progress marker left behind by the "crash"
    MARKER.write_text("commit in progress\n", encoding="utf-8")
    print(f"arranged: scratch has {SENTINEL_REL}, canonical does NOT, marker present")
    print(f"  canonical {SENTINEL_REL} exists before = {canonical_sentinel.exists()}")

    log = ROOT / "h5_be.log"
    proc = spawn_backend(PORT, log, [])   # no autostart; we drive open_project
    try:
        # backend may have no project; wait for the WS port instead of 'ready'
        deadline = time.time() + 30
        while time.time() < deadline and not port_open(PORT):
            time.sleep(0.3)
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60.0) as c:
            c.call("open_project", {"path": str(PROJ), "working_copy": True}, timeout=120)
        time.sleep(0.5)

        healed = canonical_sentinel.exists()
        marker_gone = not MARKER.exists()
        print("\n==== H5 VERDICT ====")
        print(f"canonical {SENTINEL_REL} exists after open = {healed}")
        print(f"commit marker cleared = {marker_gone}")
        if "completing interrupted commit" in read_log(log):
            print("backend logged: completing interrupted commit")
        if healed and marker_gone:
            print("=> H5 FIXED (interrupted commit rolled forward from scratch)")
        else:
            print("=> H5 NOT reproduced/fixed — investigate")
            print(read_log(log)[-800:])
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")
        # cleanup
        if SCRATCH.exists(): shutil.rmtree(SCRATCH, ignore_errors=True)
        if MARKER.exists(): MARKER.unlink()
        if canonical_sentinel.exists(): canonical_sentinel.unlink()


if __name__ == "__main__":
    raise SystemExit(main())
