"""fe_supervisor_healthy — FE-E2 (happy path) + FE-E3 (orphan guarantee).

The crash-storm test (examples/fe_supervisor) proves the FE handles a dying
backend. This proves the two properties that test can't:

  FE-E2  happy path / clean shutdown:
     A healthy project (the crasher plugin, armed:false) runs under FE autostart.
     The FE should bring the backend up, log "backend healthy", drive NO safe
     state, keep it alive, and on a Ctrl-Break shut down cleanly
     (SupervisorShutdown) with no orphaned backend.

  FE-E3  orphan guarantee:
     A *hard* kill of the FE (TerminateProcess — no console-ctrl handler runs)
     must still reap the backend via the Job Object's KILL_ON_JOB_CLOSE, so a
     supervisor crash can never leave a headless backend running the line blind.

Acceptance (mechanical): both phases hold -> PASS.

Run:  python driver.py

TODO(linux): xinsp-fe is Windows-only today (Job Object / console-ctrl /
CreateProcess). Skips on non-nt. See docs/design/linux-port.md.
"""
from __future__ import annotations

import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
EXE_SUFFIX = ".exe" if os.name == "nt" else ""
FE_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-fe{EXE_SUFFIX}"
HEALTHY_S = 8.0   # how long we require the backend to stay up untouched


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port(port: int, up: bool, budget: float) -> bool:
    deadline = time.time() + budget
    while time.time() < deadline:
        if port_open(port) == up:
            return True
        time.sleep(0.2)
    return port_open(port) == up


def launch_fe(port: int, log_path: Path) -> subprocess.Popen:
    logf = open(log_path, "wb")
    # CREATE_NEW_PROCESS_GROUP lets us send CTRL_BREAK to the FE for the clean
    # shutdown phase. (On a plain group, Ctrl-Break would hit this driver too.)
    flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    return subprocess.Popen(
        [str(FE_EXE),
         f"--port={port}",
         f"--project={ROOT}",
         "--autostart-fps=5",
         f"--be-log={ROOT / 'be.log'}"],
        cwd=str(FE_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
        creationflags=flags,
    )


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/design/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build: cmake --build backend/build --config Release --target xinsp_fe xinsp_backend")

    failures: list[str] = []

    # ===== FE-E2: happy path + clean shutdown =====================
    port = 7841
    if port_open(port):
        sys.exit(f"FAIL: :{port} already in use")
    log = ROOT / "fe.log"
    fe = launch_fe(port, log)
    print(f"[E2] FE pid={fe.pid} on :{port}")
    try:
        if not wait_port(port, up=True, budget=20):
            failures.append("E2: backend never came up under FE autostart")
        else:
            # Stay healthy + untouched for a window; backend must not die.
            t_end = time.time() + HEALTHY_S
            died = False
            while time.time() < t_end:
                if not port_open(port):
                    died = True
                    break
                if fe.poll() is not None:
                    failures.append(f"E2: FE exited unexpectedly (rc={fe.returncode}) on a healthy line")
                    break
                time.sleep(0.5)
            if died:
                failures.append("E2: backend went down during the healthy window (should not)")

            # Clean shutdown via Ctrl-Break -> the FE's console-ctrl handler.
            fe.send_signal(signal.CTRL_BREAK_EVENT)
            try:
                fe.wait(timeout=10)
            except subprocess.TimeoutExpired:
                failures.append("E2: FE did not exit within 10s of Ctrl-Break")
                fe.kill()
    finally:
        if fe.poll() is None:
            fe.kill()
            fe.wait(timeout=5)

    e2log = log.read_text(encoding="utf-8", errors="ignore")
    if "backend healthy" not in e2log:
        failures.append("E2: FE never logged 'backend healthy' (no positive liveness signal)")
    if "ENTER SAFE STATE reason=BackendExit" in e2log:
        failures.append("E2: a healthy line drove BackendExit safe-state (false alarm)")
    if "ENTER SAFE STATE reason=SupervisorShutdown" not in e2log:
        failures.append("E2: clean shutdown did not drive SupervisorShutdown safe-state")
    if "supervisor stopping" not in e2log:
        failures.append("E2: FE did not log a clean 'supervisor stopping'")
    if not wait_port(port, up=False, budget=8):
        failures.append("E2: backend still listening after clean FE shutdown (orphan)")
    print(f"[E2] {'ok' if not failures else 'issues'} — healthy window + clean shutdown")

    # ===== FE-E3: orphan guarantee (hard-kill the FE) =============
    port = 7842
    if port_open(port):
        sys.exit(f"FAIL: :{port} already in use")
    log2 = ROOT / "fe2.log"
    fe2 = launch_fe(port, log2)
    print(f"[E3] FE pid={fe2.pid} on :{port}")
    e3_fail_base = len(failures)
    try:
        if not wait_port(port, up=True, budget=20):
            failures.append("E3: backend never came up")
        else:
            # Hard kill: TerminateProcess, NOT Ctrl-Break — the FE's ctrl handler
            # never runs, so only the Job Object can reap the backend.
            fe2.kill()
            fe2.wait(timeout=5)
            print("[E3] FE hard-killed; checking backend was reaped by the Job Object")
            if not wait_port(port, up=False, budget=10):
                failures.append("E3: backend STILL listening after FE hard-kill — ORPHANED "
                                "(Job Object KILL_ON_JOB_CLOSE did not reap it)")
    finally:
        if fe2.poll() is None:
            fe2.kill()
    if len(failures) == e3_fail_base:
        print("[E3] ok — no orphan after hard FE kill")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
        print("\n---- fe.log tail ----")
        for ln in e2log.splitlines()[-20:]:
            print("  " + ln)
    else:
        print("VERDICT: PASS")
        print("  FE-E2: healthy line ran, clean Ctrl-Break shutdown, no orphan.")
        print("  FE-E3: hard FE kill reaped the backend (no orphan).")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
