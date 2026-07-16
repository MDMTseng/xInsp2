"""qa_corrupt_project_json — regression test for BUG#4: a malformed-but-nonempty
project.json must NOT be silently overwritten by a defaults rebuild on the next
instance CRUD, and the extension-owned top-level keys (params / auto_respawn /
watchdog_ms) must survive.

Background (the bug): open_project parses project.json with yyjson; on ANY syntax
error yyjson_read returns null, the loader installs DEFAULT parallelism/runtime,
pushes a warning and RETURNS true — the project opens with defaults. The next
create/remove/rename_instance calls save_project_locked, which does a FULL rebuild
from in-memory state. Its merge_unknown_top_keys_ re-reads the SAME corrupt file,
yyjson_read returns null again, the carry-over block is SKIPPED, and atomic_write
replaces project.json with a defaults-only file. Net: a single trailing comma
permanently destroys the operator's tuned `params`, `auto_respawn`, `watchdog_ms`,
AND the recoverable original bytes. In working_copy mode commit_working_copy then
mirrors the defaults file onto canonical, propagating the loss.

The fix (quarantine, not lenient-resume):
  * open_project flags project_json_malformed_ and preserves the original bytes
    verbatim as `project.json.corrupt-<ts>` (portable xi::wall_ms timestamp).
  * save_project_locked REFUSES the destructive rebuild while that flag is set —
    the project stays open degraded (like a compile failure), so project.json on
    disk is left untouched until the user fixes it and reopens.

This driver, for BOTH plain and working_copy:true opens:
  1. writes a project.json with `params:[{thresh:42}]` + `auto_respawn:true` +
     `watchdog_ms:1500` AND a trailing-comma syntax error
  2. open_project (degraded) and create_instance("cam1","mock_camera")
  3. (working_copy) commit_working_copy to mirror scratch -> canonical
  4. re-reads project.json on disk and asserts:
       - `params` / `auto_respawn` / `watchdog_ms` still present (NOT clobbered)
       - a `project.json.corrupt-*` quarantine copy exists
       - the open surfaced a "not valid JSON" warning (refusal is visible)

With the save-guard reverted, step 4 fails: create_instance's save_project_locked
overwrites project.json with defaults and the three extension keys are gone.

mock_camera is a global plugin (scanned from <repo>/plugins), so it loads even
though the malformed project.json's own `plugins` block can't parse — exactly the
realistic path by which a CRUD reaches save_project_locked on a corrupt project.

Run:  python driver.py
TODO(linux): backend plugin DLLs (mock_camera) are prebuilt Windows .dll here; SKIPs on non-nt.
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO_ROOT / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = backend_exe()
PORT = free_port()  # ephemeral (was 7874); no squatter cross-talk  # spare port — NOT the dev backend (7823) or web-terminal-hub (9091)

# A non-empty project.json with the three extension-owned top-level keys the
# backend does NOT emit, plus a deliberate trailing comma (invalid JSON).
CORRUPT_PROJECT_JSON = """{
  "name": "qa_corrupt",
  "script": "inspect.cpp",
  "params": [{"name": "thresh", "value": 42}],
  "auto_respawn": true,
  "watchdog_ms": 1500,
  "instances": [],
}
"""


def port_open(p, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout):
            return True
    except OSError:
        return False


def write_corrupt_project(folder: Path) -> None:
    if folder.exists():
        shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    (folder / "project.json").write_text(CORRUPT_PROJECT_JSON, encoding="utf-8")


def check_preserved(pj_text: str, where: str, failures: list[str]) -> None:
    """Assert the three extension-owned keys survived in `pj_text`."""
    for key in ('"thresh"', '"auto_respawn"', '"watchdog_ms"'):
        if key not in pj_text:
            failures.append(
                f"[{where}] project.json was CLOBBERED — {key} missing after CRUD "
                f"(defaults rebuild overwrote extension-owned keys)")


def has_quarantine(folder: Path) -> bool:
    return any(p.name.startswith("project.json.corrupt-") for p in folder.iterdir())


def run_case(c: Client, folder: Path, *, working_copy: bool,
             failures: list[str]) -> None:
    where = "working_copy" if working_copy else "plain"
    print(f"\n[case] {where}: open malformed project.json under {folder}")
    write_corrupt_project(folder)

    rsp = c.call("open_project", {"path": str(folder), "working_copy": working_copy},
                 timeout=120)
    print(f"[step] open_project rsp ok (degraded expected)")

    # The malformed verdict must be surfaced as an open warning.
    warns = c.call("open_project_warnings", {})
    if isinstance(warns, str):
        warns = json.loads(warns)
    reasons = " | ".join(w.get("reason", "") for w in warns.get("warnings", []))
    print(f"[step] open warnings: {reasons!r}")
    if "not valid JSON" not in reasons:
        failures.append(
            f"[{where}] open did not surface a 'not valid JSON' warning "
            f"(warnings={reasons!r}) — corruption silently resumed from defaults")

    # Trigger the destructive CRUD path: create_instance -> save_project_locked.
    cr = c.call("create_instance", {"name": "cam1", "plugin": "mock_camera"})
    print(f"[step] create_instance returned (type={type(cr).__name__})")

    if working_copy:
        # Mirror scratch -> canonical. With the fix, scratch/project.json was never
        # overwritten, so the canonical gets the original bytes back (no loss).
        com = c.call("commit_working_copy", {})
        print(f"[step] commit_working_copy returned")

    # Re-read the CANONICAL project.json from disk (folder is canonical in both
    # modes; working_copy scratch lives under folder/.xinsp_work).
    pj = (folder / "project.json").read_text(encoding="utf-8")
    print(f"[step] on-disk project.json now:\n{pj}")
    check_preserved(pj, where, failures)

    if not has_quarantine(folder):
        failures.append(
            f"[{where}] no project.json.corrupt-* quarantine copy was written "
            f"(original bytes not preserved)")
    else:
        print(f"[step] quarantine copy present: "
              f"{[p.name for p in folder.iterdir() if p.name.startswith('project.json.corrupt-')]}")

    # Close so the next case starts clean (releases the working-copy / owner lock).
    try:
        c.call("close_project", {})
    except Exception:
        pass


def cleanup() -> None:
    for sub in ("proj_plain", "proj_wc"):
        shutil.rmtree(ROOT / sub, ignore_errors=True)
    try:
        (ROOT / "be.log").unlink()
    except OSError:
        pass


def main() -> int:
    if not BE.exists():
        sys.exit(f"FAIL: missing {BE} (build xinsp_backend Release)")
    if port_open(PORT):
        sys.exit(f"FAIL: something already on :{PORT} — pick another spare port")

    cleanup()
    blog = open(ROOT / "be.log", "wb")
    be = subprocess.Popen([str(BE), f"--port={PORT}"], cwd=str(BE.parent),
                          stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    failures: list[str] = []
    try:
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(PORT):
            time.sleep(0.2)
        if not port_open(PORT):
            sys.exit("FAIL: backend did not come up")
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60) as c:
            run_case(c, ROOT / "proj_plain", working_copy=False, failures=failures)
            run_case(c, ROOT / "proj_wc",    working_copy=True,  failures=failures)
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}')
            ws.close()
        except Exception:
            pass
        try:
            be.wait(timeout=5)
        except subprocess.TimeoutExpired:
            be.kill()
        blog.close()
        cleanup()

    print("\n" + "=" * 56)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  malformed project.json was NOT clobbered on instance CRUD;")
        print("  params / auto_respawn / watchdog_ms survived and the original")
        print("  bytes were preserved as project.json.corrupt-<ts> (plain + wc).")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
