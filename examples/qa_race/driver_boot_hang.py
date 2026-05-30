"""qa_race / RACE-BOOT — FE boot-readiness gate (the achievable slice of FE-E4).

A connect-only probe can't tell a backend that's bound-but-still-booting from
one that's serving (both accept TCP). So a backend that hangs DURING autostart
(open/compile) would look "healthy" to the FE forever. The boot gate closes
that: the FE waits for the 'autostart: ready' marker and, if the backend is
alive but never reaches it within --boot-timeout-ms, treats it as a boot hang —
drives safe-state (reason=BootTimeout) and respawns.

This drives it for real: the FE forwards --hang-before-ready to the backend via
--be-arg, so every spawned backend hangs just before 'ready'. With a short
--boot-timeout-ms the FE should: detect the hang, ENTER SAFE STATE
reason=BootTimeout, respawn, and (since every boot hangs) eventually hit the
respawn cap and stay safe.

(The OTHER half of FE-E4 — a backend that wedges WHILE serving — needs a deep
WS-handshake heartbeat the FE doesn't have yet; that stays Phase 2, see
driver_fe4.py.)

Run:  python driver_boot_hang.py
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
HEALTHY_PROJECT = REPO_ROOT / "examples" / "fe_supervisor_healthy"
PORT = 7894
FE_LOG = ROOT / "fe_boot_hang.log"
BE_LOG = ROOT / "be_boot_hang.log"


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
    # Forward --hang-before-ready to every spawned backend; short boot budget so
    # the gate trips fast. (raw_thread_crash armed:false is healthy, but the
    # debug hook hangs the BE before it ever reaches 'ready'.)
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={HEALTHY_PROJECT}",
         f"--be-log={BE_LOG}",
         "--boot-timeout-ms=4000",
         "--be-arg=--hang-before-ready"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    print(f"[boot-hang] FE pid={proc.pid} on :{PORT}; every BE will hang before ready")

    # The FE self-terminates after the respawn cap (every boot hangs). Wait.
    deadline = time.time() + 90
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
    rc = proc.poll()
    if rc is None:
        print("[boot-hang] FE didn't exit; terminating")
        proc.kill()
    fe_log.close()

    log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe log tail ----")
    for ln in log.splitlines()[-18:]:
        print("  " + ln)
    print("---------------------")

    failures: list[str] = []
    if "did not reach 'autostart: ready'" not in log:
        failures.append("FE never detected the boot hang (no boot-timeout message)")
    if "ENTER SAFE STATE reason=BootTimeout" not in log:
        failures.append("FE did not drive a BootTimeout safe state")
    if "respawning backend" not in log:
        failures.append("FE did not respawn after the boot hang")
    if "reason=RespawnLimitExceeded" not in log:
        failures.append("FE did not eventually hit the cap (every boot hangs)")
    # No orphan: the hung BE must be reaped.
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
        print("  boot hang detected -> BootTimeout safe-state -> respawn -> cap, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
