"""H4 PoC — dead-man is DISCARDED if the PLC link is down at the moment the
backend crashes (comms_main.cpp:235 `&& plc.up`, then :244 clears it, no retry).

Uses a controllable TCP mock-PLC so we can force plc.up=false.

CONTROL : PLC up, client drops w/o bye  -> PLC must receive the dead-man.
H4 TEST : PLC link down, client drops w/o bye, PLC later reconnects
          -> dead-man is gone forever (never delivered).
"""
from __future__ import annotations
import os, socket, subprocess, sys, threading, time
from pathlib import Path
from harness import REPO_ROOT, COMMS_EXE

LOOP = 7905


class MockPLC:
    """A TCP server the gateway connects to as its 'PLC'. We can drop the link
    and refuse reconnects (plc down), then reopen to watch for any late bytes."""
    def __init__(self):
        self.srv = None
        self.conn = None
        self.port = 0
        self.recv_log: list[bytes] = []
        self._stop = False
        self._th = None

    def listen(self):
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        # First call: ephemeral port. reopen(): bind the SAME port so the
        # gateway's reconnect (which targets the original port) actually lands.
        self.srv.bind(("127.0.0.1", self.port))
        self.port = self.srv.getsockname()[1]
        self.srv.listen(1)
        self.srv.settimeout(0.5)

    def _accept_loop(self):
        while not self._stop:
            try:
                c, _ = self.srv.accept()
            except (socket.timeout, OSError):
                continue
            self.conn = c
            c.settimeout(0.3)
            while not self._stop:
                try:
                    d = c.recv(4096)
                    if not d:
                        break
                    self.recv_log.append(d)
                except socket.timeout:
                    continue
                except OSError:
                    break

    def start(self):
        self._th = threading.Thread(target=self._accept_loop, daemon=True)
        self._th.start()

    def drop_and_close(self):
        """Drop the PLC link AND stop listening so gateway reconnects fail."""
        if self.conn:
            try: self.conn.close()
            except OSError: pass
            self.conn = None
        try: self.srv.close()
        except OSError: pass

    def reopen(self):
        self._stop = False
        self.listen()
        self.start()

    def stop(self):
        self._stop = True
        try:
            if self.conn: self.conn.close()
        except OSError: pass
        try:
            if self.srv: self.srv.close()
        except OSError: pass


def spawn_gw(plc_port: int, log_path: Path):
    logf = open(log_path, "wb")
    return subprocess.Popen(
        [str(COMMS_EXE), f"--plc=tcp:127.0.0.1:{plc_port}", f"--listen={LOOP}"],
        cwd=str(COMMS_EXE.parent), stdout=logf, stderr=logf, stdin=subprocess.DEVNULL,
    )


def loop_connect(timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return socket.create_connection(("127.0.0.1", LOOP), timeout=0.5)
        except OSError:
            time.sleep(0.2)
    return None


def read_line(sock, want, budget=4.0):
    sock.settimeout(0.4)
    buf = b""
    end = time.time() + budget
    while time.time() < end:
        try:
            ch = sock.recv(4096)
            if not ch: break
            buf += ch
        except socket.timeout:
            pass
        if want.encode() in buf:
            return buf.decode("utf-8", "replace")
    return buf.decode("utf-8", "replace") if buf else None


def run_phase(label: str, drop_plc: bool, plc: MockPLC, log: Path):
    """Arm dead-man, optionally drop the PLC, then crash the client (no bye)."""
    c = loop_connect()
    if c is None:
        return f"{label}: could not connect to gateway loopback"
    # wait for the plc_up status push, then arm
    read_line(c, "plc_up", budget=3.0)
    c.sendall(b'{"id":1,"op":"set_deadman","line":"EMERGENCY_STOP_%s"}\n' % label.encode())
    read_line(c, '"ok":true', budget=3.0)

    if drop_plc:
        plc.drop_and_close()
        time.sleep(2.5)   # let gateway see the drop (plc.up=false) + fail a reconnect

    n_before = sum(len(x) for x in plc.recv_log)
    c.close()             # ABRUPT drop, no "bye" == backend crash
    time.sleep(1.5)

    if drop_plc:
        # give the PLC a chance to come back and receive any retried dead-man
        plc.reopen()
        time.sleep(3.5)

    blob = b"".join(plc.recv_log)
    delivered = b"EMERGENCY_STOP_%s" % label.encode() in blob
    return delivered


def main() -> int:
    if os.name != "nt":
        print("SKIP: xinsp-comms Windows-only"); return 0
    if not COMMS_EXE.exists():
        print("SKIP: comms exe missing"); return 0

    # ---- CONTROL: PLC stays up ----
    plc = MockPLC(); plc.listen(); plc.start()
    log = Path(__file__).resolve().parent / "h4_gw_control.log"
    gw = spawn_gw(plc.port, log)
    time.sleep(1.5)
    ctrl = run_phase("CTRL", drop_plc=False, plc=plc, log=log)
    try: gw.terminate(); gw.wait(timeout=4)
    except Exception: gw.kill()
    plc.stop()
    print(f"CONTROL (plc up, crash): dead-man delivered = {ctrl}")

    time.sleep(0.5)

    # ---- H4: PLC down at crash time ----
    plc2 = MockPLC(); plc2.listen(); plc2.start()
    log2 = Path(__file__).resolve().parent / "h4_gw_test.log"
    gw2 = spawn_gw(plc2.port, log2)
    time.sleep(1.5)
    h4 = run_phase("H4", drop_plc=True, plc=plc2, log=log2)
    try: gw2.terminate(); gw2.wait(timeout=4)
    except Exception: gw2.kill()
    plc2.stop()
    print(f"H4 TEST (plc down, crash, plc reconnects): dead-man delivered = {h4}")

    print("\n==== H4 VERDICT ====")
    if ctrl is True and h4 is False:
        print("CONTROL fired, H4 did NOT -> dead-man LOST when PLC down at crash time")
        print("=> H4 (dead-man discarded on PLC-down crash, no retry) CONFIRMED")
    elif ctrl is not True:
        print(f"control did not fire ({ctrl!r}) -> setup問題, H4 inconclusive")
    else:
        print(f"H4 dead-man was delivered ({h4!r}) -> NOT reproduced (retry exists?)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
