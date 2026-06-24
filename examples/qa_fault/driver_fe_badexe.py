"""qa_fault — FE with a missing/bad --backend exe (FE-E6).

If the FE cannot even spawn the backend (bad path -> CreateProcess fails), it
must record the line as DOWN — it can't leave the line running with no inspector
and no supervisor — and then exit nonzero rather than silently spinning. Per
fe_main.cpp: spawn failure -> 'CreateProcess failed' + fe-status state="down"
reason="BackendExit", rc=1, no respawn loop (there's nothing to respawn).

Asserts from fe.log + fe-status.json + the FE exit code:
  - the spawn-failure path logged: a `CreateProcess failed` line
  - fe-status.json last_event reflects reason=="BackendExit" (the FE then exits,
    so the top-level state/reason settle to stopped/SupervisorShutdown)
  - the FE exited NONZERO (documented rc=1)
  - the FE never logged a respawn (no backend to respawn)
  - nothing left listening on the private port (no orphan; none was spawned)

Run from anywhere:  python driver_fe_badexe.py

TODO(linux): xinsp-fe is Windows-only today. SKIPs on non-nt.
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
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
PORT = 7873
FE_LOG = ROOT / "fe_badexe.log"
BE_LOG = ROOT / "be_badexe.log"          # status/crash-history default next to this
STATUS_FILE = ROOT / "fe-status.json"
BAD_BACKEND = ROOT / "no_such_backend_zzz.exe"
MAX_WAIT_S = 30.0


def port_open(port: int = PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release --target xinsp_fe")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

    STATUS_FILE.unlink(missing_ok=True)

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--backend={BAD_BACKEND}",   # does not exist -> CreateProcess fails
         f"--project={ROOT / 'heal_project'}",
         f"--be-log={BE_LOG}",         # so fe-status.json lands next to it (ROOT)
         "--autostart-fps=0"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} with a bad --backend")

    deadline = time.time() + MAX_WAIT_S
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.25)
    rc = proc.poll()
    if rc is None:
        print("[fe] did not exit on a spawn failure; terminating")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    fe_log.close()

    log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe.log tail ----")
    for ln in log.splitlines()[-15:]:
        print("  " + ln)
    print("---------------------")

    failures: list[str] = []
    lines = log.splitlines()

    if not any("CreateProcess failed" in ln for ln in lines):
        failures.append("no 'CreateProcess failed' line — FE didn't report the spawn failure")
    if any("respawning backend" in ln for ln in lines):
        failures.append("FE tried to respawn — there was no backend to respawn")

    # fe-status.json records the spawn failure: the FE publishes state="down"
    # reason="BackendExit" then exits, so the FINAL top-level state/reason settle
    # to stopped/SupervisorShutdown — but last_event preserves the BackendExit.
    if STATUS_FILE.exists():
        try:
            st = json.loads(STATUS_FILE.read_text(encoding="utf-8", errors="ignore"))
            print(f"---- fe-status.json ----\n  {json.dumps(st)}")
            if st.get("state") not in ("down", "stopped"):
                failures.append(f"fe-status state not down/stopped on spawn failure: {st.get('state')!r}")
            le = st.get("last_event") or {}
            if le.get("reason") != "BackendExit":
                failures.append(f"fe-status last_event reason not 'BackendExit' on spawn failure: {le.get('reason')!r}")
        except json.JSONDecodeError as e:
            failures.append(f"fe-status.json is not valid JSON: {e}")
    else:
        failures.append(f"no fe-status.json written ({STATUS_FILE}) on spawn failure")

    if rc is None:
        failures.append("FE did not exit on a spawn failure (it must not spin)")
    elif rc == 0:
        failures.append(f"FE exited 0 on a spawn failure — must be nonzero (expected 1)")
    elif rc != 1:
        print(f"[note] FE exit code was {rc} (expected 1 for the spawn-failure path)")

    time.sleep(0.5)
    if port_open():
        failures.append(f"something is listening on :{PORT} — a backend leaked despite the bad path")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  bad --backend -> CreateProcess failed -> fe-status down")
        print("  (BackendExit), did not respawn, exited nonzero, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
