"""comms_gateway Inc1 — round-trip through the standalone xinsp-comms gateway.

Stands up a UDP "PLC simulator", launches the gateway pointed at it, connects a
loopback "backend client", and exercises both directions:
  - client sends {"id":1,"op":"send","line":"..."} -> gateway relays to the PLC,
    the PLC sim receives it, and the client gets {"id":1,"ok":true};
  - the PLC sim sends a line back -> the client receives {"event":"plc_in","line":...}.

This proves the gateway in isolation (no backend/script integration yet).

Run:  python driver.py
TODO(linux): xinsp-comms is Windows-only today; SKIPs on non-nt.
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO_ROOT = ROOT.parents[1]
EXE = REPO_ROOT / "backend" / "build" / "Release" / ("xinsp-comms.exe" if os.name == "nt" else "xinsp-comms")
GW_PORT = 7901


def read_lines(sock, want_substr, budget=5.0):
    """Read newline-framed lines until one contains want_substr (or timeout)."""
    sock.settimeout(0.4)
    buf = b""
    deadline = time.time() + budget
    while time.time() < deadline:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            pass
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode("utf-8", "replace").strip()
            if s and want_substr in s:
                return s
    return None


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-comms is Windows-only today (see docs/design/comms-gateway.md)")
        return 0
    if not EXE.exists():
        sys.exit(f"FAIL: xinsp-comms not found: {EXE}\n"
                 f"build: cmake --build backend/build --config Release --target xinsp_comms")

    plc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    plc.bind(("127.0.0.1", 0))
    plc_port = plc.getsockname()[1]
    plc.settimeout(5.0)
    print(f"[plc-sim] udp 127.0.0.1:{plc_port}")

    log = open(ROOT / "gw.log", "wb")
    gw = subprocess.Popen(
        [str(EXE), f"--plc=udp:127.0.0.1:{plc_port}", f"--listen={GW_PORT}"],
        cwd=str(EXE.parent), stdout=log, stderr=log, stdin=subprocess.DEVNULL,
    )
    failures: list[str] = []
    client = None
    try:
        # connect the loopback "backend client"
        deadline = time.time() + 10
        while time.time() < deadline and client is None:
            try:
                client = socket.create_connection(("127.0.0.1", GW_PORT), timeout=0.5)
            except OSError:
                time.sleep(0.2)
        if client is None:
            failures.append("could not connect to the gateway listen port")
            raise SystemExit  # to finally

        # client -> PLC
        client.sendall(b'{"id":1,"op":"send","line":"HELLO_PLC"}\n')
        data, peer = plc.recvfrom(4096)
        got = data.decode("utf-8", "replace").strip()
        print(f"[plc-sim] received from gateway: {got!r}")
        if got != "HELLO_PLC":
            failures.append(f"PLC did not receive the relayed line (got {got!r})")
        ack = read_lines(client, '"ok":true')
        print(f"[client] ack: {ack}")
        if not ack or json.loads(ack).get("id") != 1:
            failures.append("client did not get {id:1, ok:true} ack")

        # PLC -> client (reply to the source addr the datagram came from)
        plc.sendto(b'{"trigger":42}\n', peer)
        ev = read_lines(client, '"plc_in"')
        print(f"[client] inbound: {ev}")
        if not ev:
            failures.append("client did not receive a plc_in event")
        else:
            m = json.loads(ev)
            if m.get("event") != "plc_in" or "trigger" not in m.get("line", ""):
                failures.append(f"plc_in payload wrong: {ev}")
    finally:
        if client:
            try: client.close()
            except OSError: pass
        gw.terminate()
        try: gw.wait(timeout=5)
        except subprocess.TimeoutExpired: gw.kill()
        log.close(); plc.close()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  client->PLC relayed + acked; PLC->client delivered as plc_in.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
