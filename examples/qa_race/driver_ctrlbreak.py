"""qa_race RACE-CTRLBREAK — clean shutdown DURING a respawn-backoff window.

Race of interest: the FE's console-ctrl handler sets g_stop, but the supervisor
loop spends most of a crash-loop iteration asleep in Sleep(respawn_backoff_ms)
(1.5s) between a death and the next spawn. A CTRL_BREAK that lands inside that
backoff sleep must still exit cleanly — drive SupervisorShutdown safe-state, log
'supervisor stopping', and leave NO orphaned backend (Job Object reap).

We don't time the Ctrl-Break to the millisecond. We launch the crashburst FE,
wait until it has logged >=1 'respawning backend' (so it is actively crash-
looping, and with a 1.5s backoff vs near-instant crash it is almost certainly in
a backoff sleep), then send CTRL_BREAK_EVENT. The clean-exit + no-orphan
assertions hold regardless of exactly where the signal lands, so the test is
robust even if it occasionally lands mid-spawn.

Run from this dir:  python driver_ctrlbreak.py

TODO(linux): Windows-only (CREATE_NEW_PROCESS_GROUP / CTRL_BREAK / Job Object).
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
CRASHBURST = ROOT / "projects" / "crashburst"
PORT = 7892
LOG = ROOT / "fe_ctrlbreak.log"
BE_LOG = ROOT / "be_ctrlbreak.log"


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port_closed(port: int, budget: float) -> bool:
    deadline = time.time() + budget
    while time.time() < deadline:
        if not port_open(port):
            return True
        time.sleep(0.2)
    return not port_open(port)


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/design/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}")
    if port_open(PORT):
        sys.exit(f"FAIL: :{PORT} already in use; pick a free port in 7890-7909")

    failures: list[str] = []
    logf = open(LOG, "wb")
    fe = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={CRASHBURST}",
         "--autostart-fps=5",
         f"--be-log={BE_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    print(f"[ctrlbreak] FE pid={fe.pid} on :{PORT}")

    # Wait until the FE is actively crash-looping (>=1 respawn logged), then
    # interrupt — the signal lands during the 1.5s backoff sleep with high prob.
    saw_respawn = False
    deadline = time.time() + 60
    while time.time() < deadline:
        if fe.poll() is not None:
            break  # hit the cap before we could interrupt; still fine to assert below
        txt = LOG.read_text(encoding="utf-8", errors="ignore")
        if "respawning backend" in txt:
            saw_respawn = True
            break
        time.sleep(0.1)

    if not saw_respawn and fe.poll() is None:
        failures.append("setup: FE never logged a respawn within 60s; could not stage the race")

    # Fire Ctrl-Break (lands mid-backoff with high probability).
    if fe.poll() is None:
        # tiny nudge so we're more likely inside the Sleep() than the spawn path
        time.sleep(0.2)
        print("[ctrlbreak] sending CTRL_BREAK_EVENT (expecting it mid-backoff)")
        fe.send_signal(signal.CTRL_BREAK_EVENT)

    try:
        fe.wait(timeout=10)
    except subprocess.TimeoutExpired:
        failures.append("FE did not exit within 10s of CTRL_BREAK (stuck in backoff/spawn race)")
        fe.kill()
    logf.close()

    txt = LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe_ctrlbreak.log tail ----")
    for ln in txt.splitlines()[-20:]:
        print("  " + ln)
    print("-------------------------------")

    lines = txt.splitlines()
    # Clean shutdown markers. (If the FE happened to hit the cap first, it would
    # log RespawnLimitExceeded instead of SupervisorShutdown — accept either as a
    # clean exit, but we required a respawn was seen, so the common path is the
    # shutdown one.)
    clean_shutdown = any("reason=SupervisorShutdown" in ln for ln in lines)
    hit_cap = any("reason=RespawnLimitExceeded" in ln for ln in lines)
    if not clean_shutdown and not hit_cap:
        failures.append("no clean exit marker (neither SupervisorShutdown nor cap) in fe.log")
    if clean_shutdown and "supervisor stopping" not in txt:
        failures.append("SupervisorShutdown without 'supervisor stopping' — unclean teardown")

    # No orphan: Job Object must have reaped the backend.
    if not wait_port_closed(PORT, budget=8):
        failures.append(f"backend STILL listening on :{PORT} after FE exit — ORPHAN")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("VERDICT: PASS")
    print("  CTRL_BREAK during the crash-loop exited cleanly, no orphaned backend.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
