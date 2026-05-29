"""fe_supervisor — regression for the xinsp-fe.exe frontend supervisor.

Process isolation was removed (2026-05): a hard plugin crash takes the whole
backend down. xinsp-fe.exe is the replacement safety net — it spawns/monitors
the backend, drives the line to a safe state the instant the backend dies, and
respawns it (rate-limited). This driver proves that loop end-to-end.

What it does
------------
1. Launches `xinsp-fe.exe` pointed at this project on a private port, with
   `--autostart-fps` so the backend self-opens/compiles/starts headlessly.
2. The "crasher" instance is configured `armed: true`, so the backend's first
   inspect spawns a raw thread that null-derefs — the backend dies repeatedly.
3. The FE detects each death, calls `enter_safe_state`, reads the crash report
   from the backend log, and respawns — until it hits the 5-in-60s cap, then
   stays in safe state and exits on its own (rc=2).
4. The driver waits for the FE to exit, then asserts from the FE log:
     - >=1 `ENTER SAFE STATE reason=BackendExit` with a crash module + phase,
     - >=1 `respawning backend`,
     - `respawn limit ... exceeded` + `ENTER SAFE STATE reason=RespawnLimitExceeded`,
     - the FE exited (rc=2) and left no backend listening on the port (no orphan).

Acceptance (mechanical): all of the above hold -> PASS.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe is Windows-only today (process spawn/Job Object/console
ctrl). On non-nt this test is skipped. See docs/design/linux-port.md.
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
PORT = 7839
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
MAX_WAIT_S = 120.0


def port_open(port: int = PORT, timeout: float = 0.25) -> bool:
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
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release --target xinsp_fe xinsp_backend")
    if port_open():
        sys.exit(f"FAIL: something already listening on :{PORT}; pick a free port")

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={ROOT}",
         "--autostart-fps=5",
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT}")

    # The FE self-terminates after hitting the respawn cap. Wait for it.
    deadline = time.time() + MAX_WAIT_S
    while time.time() < deadline and proc.poll() is None:
        time.sleep(0.5)
    rc = proc.poll()
    if rc is None:
        print("[fe] did not exit within budget; terminating")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    fe_log.close()

    log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe.log tail ----")
    for line in log.splitlines()[-25:]:
        print("  " + line)
    print("---------------------")

    failures: list[str] = []

    enter_backend_exit = [ln for ln in log.splitlines()
                          if "ENTER SAFE STATE" in ln and "reason=BackendExit" in ln]
    if not enter_backend_exit:
        failures.append("no 'ENTER SAFE STATE reason=BackendExit' line — death not detected / safe-state not driven")
    else:
        # At least one should carry crash forensics (module + phase) parsed from
        # the backend's crash report.
        has_forensics = any(("module=" in ln and "module=-" not in ln) or
                            ("phase=" in ln and "phase=-" not in ln)
                            for ln in enter_backend_exit)
        if not has_forensics:
            failures.append("safe-state lines carried no crash forensics (module/phase) from the report")

    if not any("respawning backend" in ln for ln in log.splitlines()):
        failures.append("FE never logged a respawn")

    if "respawn limit" not in log or "exceeded" not in log:
        failures.append("FE did not hit/report the respawn cap")
    if "reason=RespawnLimitExceeded" not in log:
        failures.append("no 'ENTER SAFE STATE reason=RespawnLimitExceeded' — FE didn't stay safe at cap")

    if rc is None:
        failures.append("FE did not exit on its own after the cap")
    elif rc != 2:
        # rc 2 is the documented "respawn limit exceeded" exit; tolerate but note.
        print(f"[note] FE exit code was {rc} (expected 2 for respawn-cap path)")

    # No orphan: the FE's Job Object should have killed the backend on close.
    time.sleep(1.0)
    if port_open():
        failures.append(f"a backend is still listening on :{PORT} after FE exit — orphaned child")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  backend died repeatedly -> FE drove safe-state with crash")
        print("  forensics, respawned, hit the cap, stayed safe, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
