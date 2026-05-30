"""status_demo — the script status channel + its delivery guarantee.

A script calls xi::status("inspected frame N") each run. The backend keeps it
last-write-wins under "@script" and serves it via cmd:status. This driver
proves:
  1. after a run, cmd:status reports "@script" with the script's latest text;
  2. THE GUARANTEE: a brand-new client (reconnect) re-pulls cmd:status and still
     gets the latest status — it's retained in the backend, not lost on the
     disconnect that would drop any push events;
  3. coalescing: re-running with the same text doesn't bump the seq (no spam).

Run:  python driver.py
TODO(linux): spawns xinsp-backend.exe; SKIPs on non-nt.
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
SDK = REPO_ROOT / "tools" / "xinsp2_py"
sys.path.insert(0, str(SDK))
from xinsp2 import Client  # noqa: E402

EXE_SUFFIX = ".exe" if os.name == "nt" else ""
BACKEND_EXE = REPO_ROOT / "backend" / "build" / "Release" / f"xinsp-backend{EXE_SUFFIX}"
PORT = 7845
WS_URL = f"ws://127.0.0.1:{PORT}/"


def port_open(port: int, timeout: float = 0.3) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=timeout):
            return True
    except OSError:
        return False


def main() -> int:
    if os.name != "nt":
        print("SKIP: backend is Windows-only here (see docs/design/linux-port.md)")
        return 0
    if not BACKEND_EXE.exists():
        sys.exit(f"FAIL: backend not found: {BACKEND_EXE}")
    if port_open(PORT):
        sys.exit(f"FAIL: :{PORT} already in use")

    log = open(ROOT / "backend.log", "wb")
    be = subprocess.Popen([str(BACKEND_EXE), f"--port={PORT}"],
                          cwd=str(BACKEND_EXE.parent),
                          stdout=log, stderr=log, stdin=subprocess.DEVNULL)
    failures: list[str] = []
    try:
        deadline = time.time() + 15
        while time.time() < deadline and not port_open(PORT):
            time.sleep(0.2)

        # ---- session 1: open + compile + run, then read status ----
        with Client(url=WS_URL, timeout=60.0) as c:
            c.open_project(str(ROOT), timeout=300)
            c.compile_and_load(str(ROOT / "inspect.cpp"))
            c.run(timeout=30)
            s = c.status()
            print("status after run:", s)
            sc = s.get("@script")
            if not sc:
                failures.append("no '@script' status after run")
            else:
                if "inspected frame" not in sc.get("text", ""):
                    failures.append(f"@script text unexpected: {sc.get('text')!r}")
                if sc.get("seq", 0) < 1:
                    failures.append("@script seq not set")
                seq1 = sc.get("seq")
                # coalescing: same text again -> seq unchanged
                c.run(timeout=30)
                seq2 = c.status().get("@script", {}).get("seq")
                if seq2 != seq1:
                    failures.append(f"coalescing failed: seq changed on identical status ({seq1}->{seq2})")

        # ---- session 2 (reconnect): THE GUARANTEE — latest re-pulled ----
        with Client(url=WS_URL, timeout=30.0) as c2:
            s2 = c2.status()
            print("status after reconnect:", s2)
            sc2 = s2.get("@script")
            if not sc2 or "inspected frame" not in sc2.get("text", ""):
                failures.append("reconnect did NOT re-deliver the retained @script status "
                                 "(delivery guarantee broken)")
    finally:
        try:
            import websocket
            ws = websocket.create_connection(WS_URL, timeout=2)
            ws.send('{"type":"cmd","id":999,"name":"shutdown"}'); ws.close()
        except Exception:
            pass
        try:
            be.wait(timeout=5)
        except subprocess.TimeoutExpired:
            be.kill()
        log.close()

    print("\n" + "=" * 48)
    if failures:
        print("VERDICT: FAIL")
        for f in failures:
            print(f"  - {f}")
    else:
        print("VERDICT: PASS")
        print("  script status published, retained, re-delivered on reconnect, coalesced.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
