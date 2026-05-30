"""fe_comms — regression for the FE supervising the comms gateway (Increment 3).

Increments 1+2 built the gateway (xinsp-comms) and the backend-side xi::comms
client. This driver proves the last piece: xinsp-fe.exe spawns AND supervises the
gateway as a sibling of the backend, drives a CommsLost safe-state when the
gateway dies, and respawns it (rate-limited) — with the backend surviving.

What it does
------------
1. Stands up a fake UDP "PLC" sim.
2. Launches `xinsp-fe.exe` with `--comms-plc=udp:<sim>` + `--autostart-fps`, so the
   FE brings up BOTH the gateway and the backend (which self-opens/compiles/starts
   a script using xi::comms) and passes the backend `--comms-port`.
3. Asserts the script's per-frame send() reaches the PLC sim through the
   FE-spawned gateway (script -> gateway -> PLC).
4. Kills ONLY the gateway process (name-guarded: must be xinsp-comms.exe, matched
   by our unique --listen port) and asserts from the FE log:
     - `comms gateway exited`,
     - `ENTER SAFE STATE reason=CommsLost`,
     - `respawning comms gateway`,
     - `comms gateway back up`.
5. Asserts the backend SURVIVED the gateway death (its port stays up), a fresh
   gateway is running, and the script's sends reach the PLC sim again (link
   restored).
6. Stops the FE and asserts no orphans (backend port closed, no leftover gateway).

Acceptance (mechanical): all of the above hold -> PASS.

Run from this dir:  python driver.py

TODO(linux): xinsp-fe / xinsp-comms are Windows-only today (process spawn / Job
Object / console ctrl). On non-nt this test is skipped. See docs/design/linux-port.md.
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
PORT = 7855            # backend WS port
COMMS_PORT = 7856      # BE <-> gateway loopback port (also our kill-match marker)
FE_LOG = ROOT / "fe.log"
BE_LOG = ROOT / "be.log"
GW_LOG = ROOT / "gw.log"
GW_NAME = "xinsp-comms.exe"


def port_open(port: int, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def gateway_pids() -> list[int]:
    """PIDs of running gateways that belong to THIS test.

    Name-guarded by construction: the CIM filter only matches xinsp-comms.exe,
    and we further require our unique --listen port in the command line so we
    never touch an unrelated process (honors the 'verify before kill' rule)."""
    if os.name != "nt":
        return []
    ps = (f"Get-CimInstance Win32_Process -Filter \"Name='{GW_NAME}'\" | "
          f"Where-Object {{ $_.CommandLine -like '*--listen={COMMS_PORT}*' }} | "
          f"ForEach-Object {{ $_.ProcessId }}")
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                         capture_output=True, text=True)
    return [int(x) for x in out.stdout.split() if x.strip().isdigit()]


def kill_pid_named(pid: int, expect_name: str) -> bool:
    """taskkill a PID only after re-verifying its image name (defense in depth)."""
    out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True)
    if expect_name.lower() not in out.stdout.lower():
        print(f"[guard] PID {pid} is not {expect_name} ({out.stdout.strip()!r}); refusing to kill")
        return False
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True, text=True)
    return True


def drain_plc(sim: socket.socket, want: str, budget_s: float) -> bool:
    """True if a datagram containing `want` arrives within the budget."""
    deadline = time.time() + budget_s
    sim.settimeout(0.5)
    while time.time() < deadline:
        try:
            data, _ = sim.recvfrom(4096)
        except socket.timeout:
            continue
        if want in data.decode("utf-8", "replace"):
            return True
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
         "--autostart-fps=4",
         f"--comms-plc=udp:127.0.0.1:{plc_port}",
         f"--comms-port={COMMS_PORT}",
         f"--be-log={BE_LOG}",
         f"--comms-log={GW_LOG}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
    )
    print(f"[fe] launched pid={proc.pid} port={PORT} comms-port={COMMS_PORT} plc={plc_port}")

    failures: list[str] = []
    try:
        # ---- bring-up: backend serving + script's send reaching the PLC sim ----
        deadline = time.time() + 60
        while time.time() < deadline and not port_open(PORT):
            if proc.poll() is not None:
                sys.exit(f"FAIL: FE exited early rc={proc.poll()} (see {FE_LOG})")
            time.sleep(0.3)
        if not port_open(PORT):
            failures.append("backend never came up on its port")

        if not drain_plc(sim, '"result":"PASS"', budget_s=20):
            failures.append("script send() never reached the PLC sim through the FE-spawned gateway")
        else:
            print("[plc-sim] received the script's send through the gateway")

        # ---- kill ONLY the gateway, assert FE respawns it + drives CommsLost ----
        pids = gateway_pids()
        if not pids:
            failures.append("no gateway (xinsp-comms.exe) found running under the FE")
        else:
            print(f"[test] killing gateway pid(s)={pids}")
            for pid in pids:
                kill_pid_named(pid, GW_NAME)

            # FE should notice, go safe, respawn. Give it a few probe cycles.
            time.sleep(4.0)

            log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
            for needle in ("comms gateway exited",
                           "ENTER SAFE STATE reason=CommsLost",
                           "respawning comms gateway",
                           "comms gateway back up"):
                if needle not in log:
                    failures.append(f"FE log missing expected marker: {needle!r}")

            # Backend must have SURVIVED the gateway death.
            if not port_open(PORT):
                failures.append("backend went down when the gateway was killed (should be independent)")

            # A fresh gateway must be running, and its link must work again.
            if not gateway_pids():
                failures.append("FE did not respawn the gateway (none running after kill)")
            elif not drain_plc(sim, '"result":"PASS"', budget_s=15):
                failures.append("script sends did not reach the PLC sim after gateway respawn (link not restored)")
            else:
                print("[plc-sim] received sends again after gateway respawn (link restored)")
    finally:
        # ---- shutdown + orphan check ----
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

    log = FE_LOG.read_text(encoding="utf-8", errors="ignore")
    print("---- fe.log tail ----")
    for line in log.splitlines()[-25:]:
        print("  " + line)
    print("---------------------")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  FE spawned BE+gateway, script reached the PLC, gateway killed ->")
        print("  FE drove CommsLost + respawned it, BE survived, link restored, no orphan.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
