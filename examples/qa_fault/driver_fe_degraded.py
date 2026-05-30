"""qa_fault / QF-DEGRADED — a compile-broken line must NOT look healthy to the FE.

The sharpest coverage gap (test plan gap #1): the backend binds its WS port,
then compiles; if the script FAILS to compile the port is still up, so a
connect-only view would call the line "healthy" even though it can never
inspect. The fix: the backend withholds the "autostart: ready" signal and logs
"autostart: degraded" instead; the FE treats that as a failed boot — drives
safe-state and respawns (and, since a compile error is deterministic, churns to
the respawn cap and stays safe rather than silently running a blind line).

This drives it end-to-end through the FE against the deliberately-uncompilable
badscript_project fixture, and asserts:
  - the FE NEVER logged "backend healthy" (it must not trust port-up here),
  - the backend log shows "autostart: degraded" and NOT "autostart: ready",
  - the FE drove ENTER SAFE STATE (BootTimeout) and respawned to the cap,
  - no orphaned backend remained.

Run:  python driver_fe_degraded.py
TODO(linux): xinsp-fe Windows-only today; SKIPs on non-nt.
"""
from __future__ import annotations

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
BADSCRIPT_PROJECT = ROOT / "badscript_project"
PORT = 7876
FE_LOG = ROOT / "fe_degraded.log"
BE_LOG = ROOT / "be_degraded.log"


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/design/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}")
    if port_open(PORT):
        sys.exit(f"FAIL: :{PORT} already in use")

    fe_log = open(FE_LOG, "wb")
    # Short boot timeout is a safety net; the FE should trip on the explicit
    # "degraded" line well before it, so this also proves the fast path.
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={BADSCRIPT_PROJECT}",
         f"--be-log={BE_LOG}",
         "--boot-timeout-ms=30000"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    print(f"[degraded] FE pid={proc.pid} on :{PORT}; backend script won't compile")

    deadline = time.time() + 180   # 5 respawns x (compile attempt ~few s) + margin
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
    rc = proc.poll()
    if rc is None:
        print("[degraded] FE didn't exit; terminating")
        proc.kill()
    fe_log.close()

    fe = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    be = BE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe log tail ----")
    for ln in fe.splitlines()[-16:]:
        print("  " + ln)

    failures: list[str] = []
    # The crux: a non-inspecting line must NOT be reported healthy.
    if "backend healthy" in fe:
        failures.append("FE logged 'backend healthy' for a compile-broken line (must not!)")
    if "autostart: degraded" not in be:
        failures.append("backend did not log 'autostart: degraded' for the failed compile")
    if "autostart: ready" in be:
        failures.append("backend emitted 'autostart: ready' despite a failed compile (must not!)")
    if "autostart degraded" not in fe:
        failures.append("FE did not detect the degraded backend")
    if "ENTER SAFE STATE reason=BootTimeout" not in fe:
        failures.append("FE did not drive a safe state for the degraded line")
    if "respawning backend" not in fe:
        failures.append("FE did not respawn the degraded backend")
    if "reason=RespawnLimitExceeded" not in fe:
        failures.append("FE did not reach the cap (a deterministic compile error should)")
    time.sleep(1.0)
    if port_open(PORT):
        failures.append(f"a backend is still listening on :{PORT} after FE exit (orphan)")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  compile-broken line -> NOT 'healthy' -> degraded detected ->")
        print("  safe-state -> respawn -> cap, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
