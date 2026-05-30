"""qa_soak — full-stack stability soak (Phase G, #92).

The fault tests prove the supervisor reacts correctly to crashes. This proves the
inverse, which is just as important for a line: under sustained NORMAL operation
the FE must NOT trip a false safe-state, the backend must keep serving (heartbeat
advancing), and the comms link must stay up — for the whole soak, then shut down
clean with no orphan.

What it does
------------
1. Stands up a UDP "PLC" sim and launches `xinsp-fe.exe --comms-plc=udp:<sim>
   --autostart-fps=N`, bringing up the backend + gateway running a boring healthy
   script (one PLC send per frame).
2. Soaks for SOAK_S seconds (default 15; override with QA_SOAK_S). Throughout,
   samples the backend heartbeat counter and confirms the PLC sim keeps receiving.
3. Asserts:
     - the FE announced the backend healthy and the link up,
     - NO `ENTER SAFE STATE` of ANY reason (BackendExit / CommsLost / PortUnresponsive / BootTimeout),
     - NO `respawning` of the backend OR the gateway,
     - the heartbeat counter advanced by a healthy margin over the soak,
     - the PLC sim received sends in BOTH an early and a late window (link stayed up),
     - at the end the backend + exactly one gateway are still up and the FE is alive.
4. Stops the FE and asserts no orphan (backend port closed, no leftover gateway).

Run from this dir:  python driver.py     (QA_SOAK_S=30 python driver.py for longer)

TODO(linux): xinsp-fe / xinsp-comms are Windows-only today. Skips on non-nt.
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
PORT = 7865
COMMS_PORT = 7866
FPS = 5
SOAK_S = float(os.environ.get("QA_SOAK_S", "15"))
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
GW_LOG = ROOT / "gw.log"
HB_FILE = ROOT / "be.log.hb"          # FE derives the heartbeat path as be_log + ".hb"
GW_NAME = "xinsp-comms.exe"


def port_open(port: int, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def gateway_pids() -> list[int]:
    if os.name != "nt":
        return []
    ps = (f"Get-CimInstance Win32_Process -Filter \"Name='{GW_NAME}'\" | "
          f"Where-Object {{ $_.CommandLine -like '*--listen={COMMS_PORT}*' }} | "
          f"ForEach-Object {{ $_.ProcessId }}")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True)
    return [int(x) for x in out.stdout.split() if x.strip().isdigit()]


def read_hb() -> int:
    try:
        return int(HB_FILE.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return -1


def sim_received(sim: socket.socket, budget_s: float) -> bool:
    deadline = time.time() + budget_s
    sim.settimeout(0.5)
    while time.time() < deadline:
        try:
            sim.recvfrom(4096)
            return True
        except socket.timeout:
            continue
    return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe / xinsp-comms are Windows-only today (see docs/design/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}\n"
                 f"build it: cmake --build backend/build --config Release "
                 f"--target xinsp_fe xinsp_backend xinsp_comms")
    if port_open(PORT) or port_open(COMMS_PORT):
        sys.exit(f"FAIL: :{PORT} or :{COMMS_PORT} already in use; pick free ports")

    sim = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sim.bind(("127.0.0.1", 0))
    plc_port = sim.getsockname()[1]

    fe_log = open(FE_LOG, "wb")
    proc = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={ROOT}",
         f"--autostart-fps={FPS}",
         f"--comms-plc=udp:127.0.0.1:{plc_port}",
         f"--comms-port={COMMS_PORT}",
         f"--be-log={BE_LOG}",
         f"--comms-log={GW_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} soak={SOAK_S}s fps={FPS}")

    failures: list[str] = []
    try:
        # bring-up
        deadline = time.time() + 60
        while time.time() < deadline and not port_open(PORT):
            if proc.poll() is not None:
                sys.exit(f"FAIL: FE exited early rc={proc.poll()} (see {FE_LOG})")
            time.sleep(0.3)
        if not port_open(PORT):
            failures.append("backend never came up")

        # early link window
        if not sim_received(sim, budget_s=10):
            failures.append("PLC sim received nothing early — link not up at start")

        hb_start = read_hb()

        # ---- soak ----
        t_end = time.time() + SOAK_S
        while time.time() < t_end:
            if proc.poll() is not None:
                failures.append(f"FE exited mid-soak rc={proc.poll()}")
                break
            if not port_open(PORT):
                failures.append("backend dropped its port mid-soak")
                break
            time.sleep(1.0)

        hb_end = read_hb()

        # late link window
        if not sim_received(sim, budget_s=5):
            failures.append("PLC sim received nothing late — link dropped during soak")

        log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
        if "backend healthy" not in log:
            failures.append("FE never announced the backend healthy")
        if "ENTER SAFE STATE" in log:
            offenders = [ln for ln in log.splitlines() if "ENTER SAFE STATE" in ln]
            failures.append(f"FE tripped a safe-state during a healthy soak: {offenders}")
        if "respawning" in log:
            failures.append("FE respawned something during a healthy soak (false-positive death)")

        # The backend writes the heartbeat at ~1 Hz from the serving loop
        # (service_main.cpp: beat when now - last >= 1000ms), independent of fps.
        # Over the soak it should advance ~SOAK_S; require half that as slack for
        # boot + sampling. The point is it KEEPS advancing (loop not wedged).
        expected = SOAK_S * 0.5
        if hb_start < 0 or hb_end < 0:
            failures.append(f"heartbeat file unreadable (start={hb_start} end={hb_end})")
        elif (hb_end - hb_start) < expected:
            failures.append(f"heartbeat advanced only {hb_end - hb_start} over {SOAK_S}s "
                            f"(expected >= {expected:.0f}); serving loop may be stalling")
        else:
            print(f"[hb] advanced {hb_start} -> {hb_end} ({hb_end - hb_start}) over {SOAK_S}s")

        if proc.poll() is not None:
            failures.append("FE not alive at end of soak")
        if not port_open(PORT):
            failures.append("backend not up at end of soak")
        gws = gateway_pids()
        if len(gws) != 1:
            failures.append(f"expected exactly 1 gateway at end, found {len(gws)}: {gws}")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
        fe_log.close()
        sim.close()

    time.sleep(1.0)
    if port_open(PORT):
        failures.append(f"backend still listening on :{PORT} after FE exit — orphan")
    if gateway_pids():
        failures.append("a gateway is still running after FE exit — orphan")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print(f"  {SOAK_S:.0f}s healthy soak: no false safe-state, no respawn, heartbeat")
        print("  advanced, PLC link stayed up, clean shutdown, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
