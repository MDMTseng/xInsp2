"""qa_working_copy / driver_fe — FE forwards --working-copy + crash resumes scratch
(Increment 2).

Inc1 proved the BE working-copy lifecycle directly. This proves the FE layer:
the supervisor passes --working-copy to the backend it spawns, and on a crash it
respawns with the SAME flag — so the backend resumes the .xinsp_work scratch and
in-progress settings survive the crash.

What it does
------------
1. Launch `xinsp-fe.exe --project=<this> --working-copy --autostart-fps=4`.
2. Assert the scratch was seeded (.xinsp_work + be.log "working copy: seeded").
3. Attach over WS, edit the `box` instance value -> "fe_edited", save it to the
   scratch instance.json.
4. Hard-kill ONLY the backend process (name-guarded: must be xinsp-backend.exe
   on our --port). The FE respawns it.
5. Assert the respawned backend RESUMED the scratch (be.log "resuming existing")
   and the scratch instance.json still holds "fe_edited" (not re-seeded to
   "original"). Canonical stays "original" (never committed).
6. Stop the FE; assert no orphan.

Run:  python driver_fe.py

TODO(linux): xinsp-fe is Windows-only today; SKIPs on non-nt.
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
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
PORT = 7883
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
SCRATCH_INST = ROOT / ".xinsp_work" / "instances" / "box" / "instance.json"
CANON_INST = ROOT / "instances" / "box" / "instance.json"
BE_NAME = "xinsp-backend.exe"


def port_open(p=PORT, timeout=0.3):
    try:
        with socket.create_connection(("127.0.0.1", p), timeout): return True
    except OSError:
        return False


def value_of(path: Path):
    try:
        return json.loads(path.read_text()).get("config", {}).get("value")
    except (OSError, ValueError):
        return None


def backend_pids() -> list[int]:
    if os.name != "nt":
        return []
    ps = (f"Get-CimInstance Win32_Process -Filter \"Name='{BE_NAME}'\" | "
          f"Where-Object {{ $_.CommandLine -like '*--port={PORT}*' }} | "
          f"ForEach-Object {{ $_.ProcessId }}")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True)
    return [int(x) for x in out.stdout.split() if x.strip().isdigit()]


def kill_pid_named(pid: int) -> None:
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True)
    if BE_NAME.lower() not in out.stdout.lower():
        print(f"[guard] PID {pid} is not {BE_NAME}; refusing to kill")
        return
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True, text=True)


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: missing {FE_EXE} (build xinsp_fe + xinsp_backend)")
    if port_open():
        sys.exit(f"FAIL: something already on :{PORT}")

    CANON_INST.write_text(json.dumps({"plugin": "kv", "config": {"value": "original"}}, indent=2))
    shutil.rmtree(ROOT / ".xinsp_work", ignore_errors=True)

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE), f"--port={PORT}", f"--project={ROOT}", "--working-copy",
         "--autostart-fps=4", f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent), stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL)
    print(f"[fe] launched pid={proc.pid} port={PORT}")

    failures: list[str] = []
    try:
        deadline = time.time() + 60
        while time.time() < deadline and not port_open():
            if proc.poll() is not None:
                sys.exit(f"FAIL: FE exited early rc={proc.poll()} (see {FE_LOG})")
            time.sleep(0.3)
        time.sleep(1.0)  # let autostart finish open+compile

        belog1 = BE_LOG.read_text(encoding="utf-8", errors="ignore")
        if "working copy: seeded" not in belog1:
            failures.append("be.log missing 'working copy: seeded' — FE didn't pass --working-copy on first spawn")
        if value_of(SCRATCH_INST) != "original":
            failures.append(f"seeded scratch value={value_of(SCRATCH_INST)!r}, expected 'original'")

        # Edit via WS + save to the scratch.
        sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
        from xinsp2 import Client  # noqa: E402
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30) as c:
            c.set_instance_def("box", {"value": "fe_edited"})
            c.call("save_instance_config", {"name": "box"})
        if value_of(SCRATCH_INST) != "fe_edited":
            failures.append(f"after edit scratch value={value_of(SCRATCH_INST)!r}, expected 'fe_edited'")

        # Hard-kill the backend; the FE should respawn it with --working-copy.
        pids = backend_pids()
        if not pids:
            failures.append("no backend process found to kill")
        else:
            for pid in pids:
                kill_pid_named(pid)
            print(f"[test] killed backend pid(s)={pids}; waiting for FE respawn")
            # Wait for a fresh backend to come back up.
            time.sleep(2.0)
            deadline = time.time() + 60
            while time.time() < deadline and not port_open():
                time.sleep(0.3)
            time.sleep(1.0)
            belog2 = BE_LOG.read_text(encoding="utf-8", errors="ignore")
            if "working copy: resuming existing" not in belog2:
                failures.append("respawned backend did not resume the scratch "
                                "(be.log missing 'working copy: resuming existing')")
            if value_of(SCRATCH_INST) != "fe_edited":
                failures.append(f"after respawn scratch value={value_of(SCRATCH_INST)!r}, "
                                f"expected 'fe_edited' (resume should preserve it)")
        if value_of(CANON_INST) != "original":
            failures.append(f"canonical changed to {value_of(CANON_INST)!r} (never committed; must stay 'original')")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        if proc.poll() is None:
            proc.terminate()
        try: proc.wait(timeout=8)
        except subprocess.TimeoutExpired: proc.kill()
        fe_log.close()

    time.sleep(1.0)
    if port_open():
        failures.append(f"backend still on :{PORT} after FE exit — orphan")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  FE passed --working-copy; backend killed -> FE respawned it ->")
        print("  scratch resumed with the uncommitted edit, canonical untouched.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
