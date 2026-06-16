"""qa_race RACE-FE4 — PortUnresponsive (hang) path. DESIGN + manual stub.

The FE's PortUnresponsive branch (fe_main.cpp ~379-387) fires only when the BE
PROCESS IS ALIVE but the WS ACCEPT LOOP stops accepting for probe_fail_max (5)
consecutive probes (~5s at probe_interval_ms=1000): the FE logs
'backend unresponsive (N failed probes)', TerminateProcess's it, and drives
ENTER SAFE STATE reason=PortUnresponsive before respawning.

WHY THIS IS A STUB (see PLAN.md "FE-E4 ... design" for the full reasoning):
A hang plugin alone does NOT reproduce it. Continuous/`run` inspects execute on
DETACHED threads (service_main.cpp ~1615), so projects/hang's process()-sleeps-
forever wedges a DISPATCH thread while the WS poll thread keeps accepting and the
shallow connect() probe keeps SUCCEEDING. Autostart also never issues a
synchronous command, and there is no C++ WS client (Phase 2) to issue one. The
poll thread (on_text -> handle_command, service_main.cpp ~3143) only stalls if a
synchronous handler blocks on it.

FAITHFUL TRIGGER (requires a BE-side hook outside this agent's write scope):
add a debug flag, e.g. `--hang-after-boot=MS`, that makes service_main STOP
calling srv.poll() for MS *after* binding the port. Then the harness below holds:
process alive + port not accepting -> 5 probe fails -> PortUnresponsive.

This driver implements that harness shell and its assertions. It SKIPs (rc 0)
with a printed reason until the hook exists; set MANUAL_HOOK_AVAILABLE=True and
adjust HANG_FLAG once it lands.
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
HANG_PROJECT = ROOT / "projects" / "hang"
PORT = 7893
LOG = ROOT / "fe_fe4.log"
BE_LOG = ROOT / "be_fe4.log"

# Flip to True once a BE poll-stall hook (e.g. --hang-after-boot=MS) exists.
MANUAL_HOOK_AVAILABLE = False
HANG_FLAG = "--hang-after-boot=20000"   # stall the poll loop 20s after bind


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe is Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not MANUAL_HOOK_AVAILABLE:
        print("SKIP: RACE-FE4 (PortUnresponsive) needs a BE poll-stall hook that does not")
        print("      exist yet. A hang plugin alone cannot stall the WS accept loop")
        print("      (inspects run on detached threads). See qa_race/PLAN.md 'FE-E4")
        print("      design' and driver_fe4.py header. Add --hang-after-boot=MS to the")
        print("      backend, then set MANUAL_HOOK_AVAILABLE=True here.")
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
         f"--project={HANG_PROJECT}",
         "--autostart-fps=5",
         f"--be-log={BE_LOG}",
         # The FE forwards unknown trailing flags? It does not — so the hook
         # must be plumbed through the FE's build_cmdline OR set on the BE via
         # an env var the BE reads. Document this when wiring the hook.
         ],
        cwd=str(FE_EXE.parent),
        stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    print(f"[fe4] FE pid={fe.pid} on :{PORT}; expecting a poll-stall ~20s in")

    # Wait for the PortUnresponsive transition: 5 probe fails ~= 5s after the
    # stall begins. Generous budget.
    deadline = time.time() + 60
    saw = False
    while time.time() < deadline and fe.poll() is None:
        txt = LOG.read_text(encoding="utf-8", errors="ignore")
        if "reason=PortUnresponsive" in txt or "backend unresponsive" in txt:
            saw = True
            break
        time.sleep(0.5)

    # Stop the FE cleanly.
    if fe.poll() is None:
        import signal
        fe.send_signal(signal.CTRL_BREAK_EVENT)
        try:
            fe.wait(timeout=10)
        except subprocess.TimeoutExpired:
            fe.kill()
    logf.close()

    txt = LOG.read_text(encoding="utf-8", errors="ignore")
    if "backend unresponsive" not in txt:
        failures.append("FE never logged 'backend unresponsive' — probe did not fail N times")
    if "reason=PortUnresponsive" not in txt:
        failures.append("FE never drove ENTER SAFE STATE reason=PortUnresponsive")
    # PortUnresponsive must, like BackendExit, precede a respawn.
    lines = txt.splitlines()
    pu = next((i for i, ln in enumerate(lines)
               if "ENTER SAFE STATE" in ln and "reason=PortUnresponsive" in ln), None)
    rs = next((i for i, ln in enumerate(lines) if "respawning backend" in ln), None)
    if pu is not None and rs is not None and not (pu < rs):
        failures.append("PortUnresponsive safe-state did not precede the respawn (SP1 violation)")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("VERDICT: PASS")
    print("  BE alive + port wedged -> N probe fails -> PortUnresponsive safe-state -> respawn.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
