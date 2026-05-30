"""comms_script — end-to-end xi::comms through the gateway (Increment 2).

Wires the whole chain: a UDP "PLC sim" <- xinsp-comms gateway <- backend
(--comms-port) running a script that uses xi::comms::send/poll/up/set_deadman.
Asserts:
  - script send() reaches the PLC sim (script -> PLC);
  - a line the PLC sim sends comes back to the script via poll() (PLC -> script);
  - up() reports the link as up.

Run:  python driver.py
TODO(linux): xinsp-comms / backend are Windows-only here; SKIPs on non-nt.
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
sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

SUF = ".exe" if os.name == "nt" else ""
BE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{SUF}"
GW = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-comms{SUF}"
BE_PORT, GW_PORT = 7847, 7902


def port_open(p):
    try:
        with socket.create_connection(("127.0.0.1", p), 0.3): return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only today (see docs/design/comms-gateway.md)")
        return 0
    for exe in (BE, GW):
        if not exe.exists():
            sys.exit(f"FAIL: missing {exe} (build xinsp_backend + xinsp_comms)")

    plc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    plc.bind(("127.0.0.1", 0)); plc_port = plc.getsockname()[1]; plc.settimeout(5.0)
    glog = open(ROOT / "gw.log", "wb"); blog = open(ROOT / "be.log", "wb")
    gw = subprocess.Popen([str(GW), f"--plc=udp:127.0.0.1:{plc_port}", f"--listen={GW_PORT}"],
                          cwd=str(GW.parent), stdout=glog, stderr=glog, stdin=subprocess.DEVNULL)
    time.sleep(0.5)
    be = subprocess.Popen([str(BE), f"--port={BE_PORT}", f"--comms-port={GW_PORT}"],
                          cwd=str(BE.parent), stdout=blog, stderr=blog, stdin=subprocess.DEVNULL)
    failures: list[str] = []
    try:
        deadline = time.time() + 15
        while time.time() < deadline and not port_open(BE_PORT):
            time.sleep(0.2)
        with Client(url=f"ws://127.0.0.1:{BE_PORT}/", timeout=60) as c:
            c.open_project(str(ROOT), timeout=300)
            c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=180)

            # run #1: script -> PLC. The sim learns the gateway's UDP source addr.
            r1 = c.run(timeout=30)
            print("run1: plc_up =", r1.value("plc_up"), " plc_msgs =", r1.value("plc_msgs"))
            data, gw_addr = plc.recvfrom(4096)
            got = data.decode("utf-8", "replace").strip()
            print("[plc-sim] got from script:", got)
            if '"result":"PASS"' not in got:
                failures.append(f"script send() did not reach the PLC (got {got!r})")
            if r1.value("plc_up") != 1:
                failures.append("script saw plc link down (up() != 1)")

            # PLC -> script: sim pushes a line back to the gateway's source addr.
            plc.sendto(b'{"trigger":7}\n', gw_addr)
            time.sleep(0.6)   # let the gateway relay + the BE reader buffer it

            # run #2: poll() should now drain the trigger.
            r2 = c.run(timeout=30)
            print("run2: plc_msgs =", r2.value("plc_msgs"), " first_plc =", r2.value("first_plc"))
            if (r2.value("plc_msgs") or 0) < 1:
                failures.append("script poll() did not receive the PLC-pushed line")
            elif "trigger" not in str(r2.value("first_plc", "")):
                failures.append(f"poll() line wrong: {r2.value('first_plc')!r}")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(f"ws://127.0.0.1:{BE_PORT}/", timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        for p in (be, gw):
            try: p.wait(timeout=5)
            except subprocess.TimeoutExpired: p.kill()
        glog.close(); blog.close(); plc.close()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures: print("  -", f)
    else:
        print("VERDICT: PASS")
        print("  script<->PLC round trip through the gateway: send reached PLC,")
        print("  PLC push drained via poll(), link up.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
