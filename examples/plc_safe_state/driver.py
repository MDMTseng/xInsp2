"""plc_safe_state — the FE drives a real PLC transport on backend death.

The SafeStateSink can target a PLC over TCP/UDP with a newline-JSON message
(--safe-state=udp:HOST:PORT / tcp:HOST:PORT). This stands up a tiny UDP "PLC
simulator", runs the FE on the crashing project (examples/fe_supervisor) pointed
at the sim, and asserts the sim actually RECEIVES a safe-state command carrying
the crash forensics — i.e. on a plugin crash the line really would be told to go
safe.

Run:  python driver.py
TODO(linux): xinsp-fe + the PLC sink are Windows-only today; SKIPs on non-nt.
"""
from __future__ import annotations

import json
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
CRASH_PROJECT = REPO_ROOT / "examples" / "fe_supervisor"   # armed to crash on frame 0
PORT = 7896


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-fe / PLC sink are Windows-only today (see docs/roadmap/linux-port.md)")
        return 0
    if not FE_EXE.exists():
        sys.exit(f"FAIL: xinsp-fe not found: {FE_EXE}")

    # Stand up the UDP "PLC": bind an ephemeral port and listen for safe-state msgs.
    plc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    plc.bind(("127.0.0.1", 0))
    plc_port = plc.getsockname()[1]
    plc.settimeout(0.5)
    print(f"[plc-sim] listening udp 127.0.0.1:{plc_port}")

    fe_log = open(ROOT / "fe.log", "wb")
    fe = subprocess.Popen(
        [str(FE_EXE),
         f"--port={PORT}",
         f"--project={CRASH_PROJECT}",
         "--autostart-fps=5",
         f"--safe-state=udp:127.0.0.1:{plc_port}",
         f"--be-log={ROOT / 'be.log'}"],
        cwd=str(FE_EXE.parent),
        stdout=fe_log, stderr=fe_log, stdin=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )
    print(f"[fe] pid={fe.pid}; backend will crash -> FE should command the PLC")

    msgs: list[dict] = []
    raw: list[str] = []
    deadline = time.time() + 60
    while time.time() < deadline and fe.poll() is None:
        try:
            data, _ = plc.recvfrom(4096)
        except socket.timeout:
            continue
        for line in data.decode("utf-8", "replace").splitlines():
            line = line.strip()
            if not line:
                continue
            raw.append(line)
            try:
                msgs.append(json.loads(line))
            except Exception:
                pass
        # Got enough evidence (a couple of safe-state commands)? stop early.
        if sum(1 for m in msgs if m.get("state") == "enter") >= 2:
            break

    if fe.poll() is None:
        fe.send_signal(signal.CTRL_BREAK_EVENT)
        try: fe.wait(timeout=10)
        except subprocess.TimeoutExpired: fe.kill()
    fe_log.close(); plc.close()

    print(f"[plc-sim] received {len(raw)} datagram line(s)")
    for m in raw[:4]:
        print("   ", m)

    failures: list[str] = []
    enters = [m for m in msgs if m.get("event") == "safe_state" and m.get("state") == "enter"]
    if not msgs:
        failures.append("PLC sim received NO safe-state messages (transport broken)")
    if not enters:
        failures.append("no 'state':'enter' safe-state command received")
    else:
        e = enters[0]
        if e.get("reason") != "BackendExit":
            failures.append(f"enter reason={e.get('reason')!r}, expected BackendExit")
        if not e.get("module") or e.get("module") in ("", "-"):
            failures.append("enter command carried no faulting module (forensics missing)")
        if "ts_ms" not in e:
            failures.append("enter command missing ts_ms")

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print(f"  plugin crash -> FE sent safe-state JSON to the (UDP) PLC with"
              f" reason+module+phase. {len(enters)} enter command(s) received.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
