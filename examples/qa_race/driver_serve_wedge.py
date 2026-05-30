"""qa_race / RACE-WEDGE — serve-time wedge detection (FE-E4b, the last liveness gap).

A backend can be alive AND accepting TCP yet stalled mid-command (a synchronous
WS handler wedges the poll loop). A connect-only probe can't see this — it would
report the line healthy forever. The FE closes the gap with a liveness heartbeat:
the backend writes a monotonic counter from its SERVING loop, and the FE respawns
if it stalls (--heartbeat-stale-ms) while the port still accepts.

(A WS ping would conflict with the backend's single-client server when an operator
HMI holds the connection — hence the heartbeat-file approach.)

This drives it: the FE forwards --hang-after-ready to the backend (via --be-arg),
so every backend reaches "ready" (FE marks healthy), then wedges its serving loop.
With a short --heartbeat-stale-ms the FE should detect the stall, drive
PortUnresponsive safe-state, and respawn — repeatedly. (We do NOT assert the
respawn cap here: each wedge cycle is ~10-13s, so deaths land slower than the
5-per-60s window accumulates — a slow recurring fault is deliberately retried,
not treated as a burst. The headline is the detection.) We then stop the FE and
assert no orphan.

Run:  python driver_serve_wedge.py
TODO(linux): xinsp-fe Windows-only today; SKIPs on non-nt.
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
HEALTHY_PROJECT = REPO_ROOT / "examples" / "fe_supervisor_healthy"
PORT = 7895
FE_LOG = ROOT / "fe_serve_wedge.log"
BE_LOG = ROOT / "be_serve_wedge.log"


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
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={HEALTHY_PROJECT}",
         f"--be-log={BE_LOG}",
         "--heartbeat-stale-ms=4000",
         "--be-arg=--hang-after-ready"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    print(f"[serve-wedge] FE pid={proc.pid} on :{PORT}; every BE wedges after ready")

    # Watch until the FE has detected the wedge and respawned at least twice
    # (proving the loop), then stop it. Bounded so a never-capping retry loop
    # doesn't run forever.
    deadline = time.time() + 90
    while time.time() < deadline and proc.poll() is None:
        txt = FE_LOG.read_text(encoding="utf-8", errors="ignore")
        if txt.count("heartbeat stale") >= 2:
            break
        time.sleep(0.5)
    # Clean stop via Ctrl-Break (unless it already self-exited at the cap).
    if proc.poll() is None:
        proc.send_signal(signal.CTRL_BREAK_EVENT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    fe_log.close()

    log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe log tail ----")
    for ln in log.splitlines()[-16:]:
        print("  " + ln)

    failures: list[str] = []
    # The crux: the FE must mark it healthy first (port up + ready) THEN detect
    # the wedge via the stalled heartbeat — not via the boot gate.
    if "backend healthy" not in log:
        failures.append("FE never marked the backend healthy (wedge test must pass the boot gate first)")
    if "heartbeat stale" not in log:
        failures.append("FE did not detect the stalled heartbeat (serve-time wedge missed)")
    if "ENTER SAFE STATE reason=PortUnresponsive" not in log:
        failures.append("FE did not drive PortUnresponsive safe-state for the wedge")
    if "respawning backend" not in log:
        failures.append("FE did not respawn the wedged backend")
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
        print("  healthy -> serving loop wedged -> heartbeat stale -> PortUnresponsive")
        print("  -> respawn (loop) -> clean stop, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
