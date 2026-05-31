"""qa_working_copy — transactional project working copy (Increment 1, BE core).

Model: open_project with working_copy=true operates on a <project>/.xinsp_work
scratch copy. All edits land there; the canonical project is untouched until an
explicit commit. The scratch is on disk, so it survives a backend crash — on
respawn the BE resumes it. discard re-seeds the scratch from the canonical project.

This driver drives the BE directly and asserts, by reading the on-disk
instance.json in both locations, the full lifecycle on a `kv` instance whose
config is a single string `value`:

  1. open (working_copy)      -> scratch seeded; scratch value == canonical "original"
  2. edit -> "edited" + save  -> scratch == "edited", canonical STILL "original"  (isolation)
  3. kill BE, restart, open   -> scratch STILL "edited" (resumed, not re-seeded);
                                 canonical STILL "original"                       (crash-durable)
  4. commit_working_copy       -> canonical == "edited"                            (save)
  5. edit -> "edited2" + save  -> scratch "edited2", canonical "edited"
  6. discard_working_copy      -> scratch re-seeded to canonical "edited";
                                 "edited2" gone                                    (discard)

Run:  python driver.py

TODO(linux): backend plugin compile (cl.exe) is Windows-only; SKIPs on non-nt.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
PORT = 7881
SCRATCH = ROOT / ".xinsp_work"
CANON_INST = ROOT / "instances" / "box" / "instance.json"
SCRATCH_INST = SCRATCH / "instances" / "box" / "instance.json"


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def value_of(path: Path):
    try:
        return json.loads(path.read_text()).get("config", {}).get("value")
    except (OSError, ValueError):
        return None


def start_be():
    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    deadline = time.time() + 20
    while time.time() < deadline and not port_open(PORT):
        if be.poll() is not None:
            blog.close(); sys.exit(f"FAIL: BE exited during boot rc={be.poll()}")
        time.sleep(0.2)
    return be, blog


def stop_be(be, blog, *, hard):
    if hard:
        be.kill()          # simulate a crash — no clean shutdown
    else:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
    try: be.wait(timeout=6)
    except subprocess.TimeoutExpired: be.kill()
    blog.close()


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend compile is Windows-only here")
        return 0
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend)")
    if port_open(PORT):
        sys.exit(f"FAIL: something already on :{PORT}")

    # Idempotent reset: pristine canonical config + no leftover scratch.
    CANON_INST.write_text(json.dumps({"plugin": "kv", "config": {"value": "original"}}, indent=2))
    import shutil
    shutil.rmtree(SCRATCH, ignore_errors=True)

    failures: list[str] = []

    # ---- session 1: open (seed), edit, save, verify isolation ----
    be, blog = start_be()
    try:
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            rsp = c.call("open_project", {"path": str(ROOT), "working_copy": True}, timeout=300)
            if not (isinstance(rsp, dict) and rsp.get("working_copy") is True):
                failures.append(f"open did not report working_copy active: {rsp}")
            if not SCRATCH_INST.exists():
                failures.append("scratch instance.json not seeded")
            elif value_of(SCRATCH_INST) != "original":
                failures.append(f"seeded scratch value={value_of(SCRATCH_INST)!r}, expected 'original'")

            c.set_instance_def("box", {"value": "edited"})
            c.call("save_instance_config", {"name": "box"})

        if value_of(SCRATCH_INST) != "edited":
            failures.append(f"after edit+save scratch value={value_of(SCRATCH_INST)!r}, expected 'edited'")
        if value_of(CANON_INST) != "original":
            failures.append(f"ISOLATION BROKEN: canonical changed to {value_of(CANON_INST)!r} (must stay 'original')")
    finally:
        stop_be(be, blog, hard=True)   # crash, no commit

    # ---- session 2: resume after crash, commit, then discard ----
    be, blog = start_be()
    try:
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            rsp = c.call("open_project", {"path": str(ROOT), "working_copy": True}, timeout=300)
            # crash-durable: scratch must still hold the uncommitted edit.
            if value_of(SCRATCH_INST) != "edited":
                failures.append(f"after crash+resume scratch value={value_of(SCRATCH_INST)!r}, expected 'edited' "
                                f"(BE re-seeded instead of resuming?)")
            if value_of(CANON_INST) != "original":
                failures.append(f"canonical changed before commit: {value_of(CANON_INST)!r}")

            # commit -> canonical updated
            c.call("commit_working_copy", {})
            if value_of(CANON_INST) != "edited":
                failures.append(f"after commit canonical value={value_of(CANON_INST)!r}, expected 'edited'")

            # edit again (uncommitted), then discard -> back to canonical 'edited'
            c.set_instance_def("box", {"value": "edited2"})
            c.call("save_instance_config", {"name": "box"})
            if value_of(SCRATCH_INST) != "edited2":
                failures.append(f"pre-discard scratch value={value_of(SCRATCH_INST)!r}, expected 'edited2'")
            c.call("discard_working_copy", {})
            if value_of(SCRATCH_INST) != "edited":
                failures.append(f"after discard scratch value={value_of(SCRATCH_INST)!r}, expected 'edited' "
                                f"(canonical); 'edited2' should be gone")
    finally:
        stop_be(be, blog, hard=False)

    print(f"[final] canonical={value_of(CANON_INST)!r} scratch={value_of(SCRATCH_INST)!r}")
    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  edits isolated to .xinsp_work, survived a crash (resumed), commit")
        print("  pushed them to the project, discard reverted to the committed state.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
