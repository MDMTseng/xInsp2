"""qa_slow_consumer — RT8 / doc 21 §P2 end-to-end proof: a slow-but-alive WS
consumer must NOT stall the ordered inspection lane (docs/new_gen/23-rt8-async-
writer.md).

The xInsp2 Python SDK drains its socket eagerly on a background thread, so it
cannot model a slow consumer. This driver therefore speaks the WS protocol with
a tiny raw client (RawWS below) whose read rate we control precisely — the ONE
client the single-client backend serves, used for BOTH control cmds and the
throttled preview drain.

Two phases, two isolated backends (unique ephemeral ports via
qa/lib/ports.free_port):

  PHASE 1 — LANE LIVENESS + WIRE ORDER
    A continuous lane (dispatch_threads=4) pushes a modest raw preview on two
    channels each tick. The client drains SLOWLY (one frame per ~40 ms). We
    snapshot cmd:metrics.frames_total before (M0) and at the end of a fixed slow
    window (M1), and count how many preview frames the slow client actually
    received in that window. THE assertion: the backend advanced MANY more frames
    than the client drained — the lane ran free, not pinned to the socket. Every
    received XEX1 frame is in strict per-channel seq order.
      (Pre-fix, with a synchronous send under the emit gate, frames_total tracks
       the slow drain and the ratio collapses to ~1 — see the unit test's
       -DXINSP2_WS_SYNC_SEND_DEMO demonstration.)

  PHASE 2 — CLEAN DROP + CONTINUITY
    A fully-wedged client (stops reading entirely) must be dropped within a
    bounded time so the single-client slot frees; a second FAST client then
    connects and receives an in-order stream while the lane keeps serving.

Run:  python qa/qa_slow_consumer/driver.py   (Windows; backend + plugins built)
"""
from __future__ import annotations

import base64
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from xex1 import decode_xex1, is_xex1  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
INSPECT = ROOT / "inspect.cpp"


# ---------------------------------------------------------------------------
# Minimal raw WebSocket client with a *controllable* read rate.
# ---------------------------------------------------------------------------
class WsError(Exception):
    pass


class RawWS:
    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        # Socket-level recv timeout so a STARVED handoff / stalled lane surfaces as
        # a clean, diagnosable WsError (see _recv_into_buf) instead of an opaque
        # run_qa 360s TIMEOUT. 50s is generous vs the healthy ~2s handoff, so it
        # never false-trips a slow-but-alive lane under full-gate machine load, yet
        # bounds any true stall well under run_qa's 360s per-test cap.
        self.sock.settimeout(50.0)
        self._buf = bytearray()
        self._next_id = 1
        # RFC 6455 handshake (server does not verify the accept key echo).
        req = (
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.sock.sendall(req.encode("ascii"))
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = self.sock.recv(1024)
            if not chunk:
                raise WsError("handshake: peer closed")
            resp += chunk
        if b" 101 " not in resp.split(b"\r\n", 1)[0]:
            raise WsError(f"handshake not 101: {resp[:64]!r}")

    # ---- raw frame I/O ----
    def _recv_into_buf(self) -> bool:
        try:
            chunk = self.sock.recv(65536)
        except socket.timeout as e:
            # The socket-level deadline (set in __init__) fired: no bytes for 50s.
            # Convert it to a WsError so the calling phase's handler names the phase
            # (e.g. "phase2: exception: WsError(...)") — a clean, diagnosable FAIL
            # inside run_qa's window rather than an opaque 360s suite TIMEOUT. In
            # this test a live lane delivers frames continuously, so a 50s gap means
            # the lane/handoff stalled.
            raise WsError("recv timed out (50s) — lane/handoff stalled (no frames)") from e
        if not chunk:
            return False
        self._buf += chunk
        return True

    def _try_parse_frame(self):
        """Parse one server (unmasked) frame from the buffer, or None if we need
        more bytes. Returns (opcode, payload_bytes)."""
        b = self._buf
        if len(b) < 2:
            return None
        b0, b1 = b[0], b[1]
        opcode = b0 & 0x0F
        ln = b1 & 0x7F           # server never masks
        off = 2
        if ln == 126:
            if len(b) < 4:
                return None
            ln = struct.unpack_from(">H", b, 2)[0]
            off = 4
        elif ln == 127:
            if len(b) < 10:
                return None
            ln = struct.unpack_from(">Q", b, 2)[0]
            off = 10
        if len(b) < off + ln:
            return None
        payload = bytes(b[off:off + ln])
        del b[:off + ln]
        return (opcode, payload)

    def next_frame(self, throttle_s: float = 0.0):
        """Return the next (opcode, payload). When throttle_s > 0, sleep after a
        frame boundary so the socket exerts real backpressure (a slow consumer)."""
        while True:
            fr = self._try_parse_frame()
            if fr is not None:
                if throttle_s:
                    time.sleep(throttle_s)
                return fr
            if not self._recv_into_buf():
                raise WsError("peer closed")

    # ---- masked client->server text send ----
    def _send_text(self, s: str) -> None:
        data = s.encode("utf-8")
        n = len(data)
        hdr = bytearray([0x81])   # FIN + text
        if n < 126:
            hdr.append(0x80 | n)
        elif n <= 0xFFFF:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", n)
        mask = os.urandom(4)
        hdr += mask
        masked = bytes(data[i] ^ mask[i & 3] for i in range(n))
        self.sock.sendall(bytes(hdr) + masked)

    def send_cmd(self, name: str, args: dict | None = None) -> int:
        cid = self._next_id
        self._next_id += 1
        self._send_text(json.dumps({"type": "cmd", "id": cid, "name": name, "args": args or {}}))
        return cid

    def call(self, name: str, args: dict | None = None, timeout: float = 45.0):
        """Send a cmd and drain frames (fast) until its rsp arrives. Skips events,
        logs and binary preview frames in the meantime. Returns rsp['data'].

        The default is a BOUNDED 45s (compile calls pass their own longer timeout).
        This matters for the phase-2 drop/reconnect handoff: if the backend ever
        fails to promptly serve the fresh fast client, the old 300s default let a
        single control call outlast run_qa's 360s per-test cap — surfacing as an
        indefinite suite HANG (a 4-min stall was observed under contention) rather
        than a clean failure. 45s converts any such stuck handoff into a fast,
        diagnosable FAIL inside the window; the healthy path answers in ~2s."""
        cid = self.send_cmd(name, args)
        deadline = time.time() + timeout
        while True:
            if time.time() > deadline:
                raise WsError(f"cmd {name!r}: timeout waiting for rsp")
            op, payload = self.next_frame()
            if op == 0x1:  # text
                try:
                    msg = json.loads(payload.decode("utf-8"))
                except Exception:
                    continue
                if msg.get("type") == "rsp" and msg.get("id") == cid:
                    if not msg.get("ok"):
                        raise WsError(f"cmd {name!r} failed: {msg.get('error')}")
                    return msg.get("data")
            # else: event / log / binary — ignore while waiting for our rsp

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def spawn(port: int) -> subprocess.Popen:
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO))


def wait_connect(port: int) -> RawWS | None:
    for _ in range(80):
        try:
            return RawWS(port)
        except Exception:
            time.sleep(0.5)
    return None


def setup_lane(ws: RawWS, fps: int) -> None:
    ws.call("open_project", {"path": str(ROOT)})
    ws.call("compile_and_load", {"path": str(INSPECT)}, timeout=300)
    ws.call("exchange_instance", {"name": "expose",
                                  "cmd": {"command": "subscribe", "channels": ["a", "b"]}})
    ws.call("start", {"fps": fps})


def metrics_frames_total(ws: RawWS) -> int:
    data = ws.call("metrics")
    return int(data.get("frames_total", 0))


# ---------------------------------------------------------------------------
def phase1(fails: list[str]) -> None:
    port = free_port()
    proc = spawn(port)
    try:
        ws = wait_connect(port)
        if not ws:
            fails.append("phase1: could not connect"); return
        setup_lane(ws, fps=200)

        # Let the lane reach steady state, draining fast, then snapshot M0.
        t_warm = time.time() + 0.6
        while time.time() < t_warm:
            ws.next_frame()          # fast drain (no throttle)
        m0 = metrics_frames_total(ws)

        # --- SLOW WINDOW: drain ~one frame per 40 ms for T seconds ---
        T = 3.0
        throttle = 0.040
        last_seq: dict[str, int] = {}
        order_ok = True
        recv_in_window = 0
        t_end = time.time() + T
        while time.time() < t_end:
            try:
                op, payload = ws.next_frame(throttle_s=throttle)
            except WsError:
                fails.append("phase1: connection dropped during slow window "
                             "(byte-cap fired — drain too slow / window too long)")
                return
            if op == 0x2 and is_xex1(payload):
                try:
                    fr = decode_xex1(payload)
                except Exception:
                    continue
                ch, seq = fr.get("channel"), fr.get("seq")
                if ch in ("a", "b") and isinstance(seq, int):
                    recv_in_window += 1
                    prev = last_seq.get(ch)
                    if prev is not None and seq <= prev:
                        order_ok = False
                        fails.append(f"phase1: channel {ch!r} seq not strictly "
                                     f"increasing: {seq} after {prev}")
                    last_seq[ch] = seq

        # Snapshot M1 as-of the END of the slow window: the metrics value is
        # captured by the backend when it DISPATCHES the cmd (promptly, off the
        # slow send path), even though the rsp itself arrives after we start
        # draining fast below.
        m1 = metrics_frames_total(ws)
        ws.call("stop")

        delta = m1 - m0                       # backend inspections during the window
        client_ticks = recv_in_window / 2.0   # 2 channels/tick received by the slow client
        ratio = (delta / client_ticks) if client_ticks > 0 else float("inf")
        print(f"phase1: m0={m0} m1={m1} delta={delta} "
              f"client_frames={recv_in_window} client_ticks={client_ticks:.0f} "
              f"ratio={ratio:.1f} order_ok={order_ok}")

        # THE liveness assertion is the RATIO: the lane advanced far more than the
        # slow client drained. Pre-fix this ratio is ~1 (compute pinned to the
        # drain); fixed it is many-x. The ratio is load-ROBUST — under CPU load
        # both the backend delta and the client's drain fall together, so their
        # ratio is preserved. The absolute delta is only a floor guarding the
        # degenerate "almost nothing happened" case (which would make the ratio
        # meaningless); it is throughput, hence environment-dependent, so it is a
        # LOW load-tolerant floor, not a near-fps expectation. Historically the
        # tight 150 floor (25% of fps*T) was the flake — starved throughput dipped
        # under it even though the lane was plainly decoupled (ratio still high).
        if ratio < 3.0:
            fails.append(f"phase1: liveness FAIL — backend delta={delta} only "
                         f"{ratio:.1f}x the slow client's {client_ticks:.0f} ticks "
                         f"(lane pinned to the consumer; want >=3x).")
        if delta < 60:
            fails.append(f"phase1: backend advanced only {delta} frames in {T}s — "
                         f"the lane barely ran at all (degenerate window).")
        if not order_ok:
            fails.append("phase1: wire order FAIL (see per-channel messages above)")
        ws.close()
    except Exception as e:
        fails.append(f"phase1: exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()


def phase2(fails: list[str]) -> None:
    port = free_port()
    proc = spawn(port)
    try:
        wedged = wait_connect(port)
        if not wedged:
            fails.append("phase2: could not connect wedged client"); return
        setup_lane(wedged, fps=200)
        # Read a couple frames to confirm the stream is live, then WEDGE: stop
        # reading entirely. The writer's ::send fills the socket, SO_SNDTIMEO
        # fires, and the backend proactively drops us within a bounded time.
        wedged.next_frame(); wedged.next_frame()
        t_wedged = time.time()

        # A second FAST client can only connect once the wedged one is dropped
        # (single-client server 503s a 2nd connect while the first is alive).
        fast = None
        deadline = time.time() + 12.0
        while time.time() < deadline:
            try:
                fast = RawWS(port)
                break
            except Exception:
                time.sleep(0.25)
        drop_secs = time.time() - t_wedged
        if not fast:
            fails.append(f"phase2: wedged client NOT dropped within 12s — "
                         f"the single-client slot never freed."); return
        print(f"phase2: wedged client dropped + fast client connected after {drop_secs:.1f}s")
        if drop_secs > 8.0:
            fails.append(f"phase2: drop took {drop_secs:.1f}s (> 8s bound)")

        # Continuity: the fast client must get an in-order live stream while the
        # lane keeps serving (frames_total advances).
        fast.call("exchange_instance", {"name": "expose",
                                        "cmd": {"command": "subscribe", "channels": ["a", "b"]}})
        f0 = metrics_frames_total(fast)
        last_seq: dict[str, int] = {}
        got = 0
        order_ok = True
        t_end = time.time() + 1.5
        while time.time() < t_end and got < 60:
            try:
                op, payload = fast.next_frame()
            except WsError:
                break
            if op == 0x2 and is_xex1(payload):
                try:
                    fr = decode_xex1(payload)
                except Exception:
                    continue
                ch, seq = fr.get("channel"), fr.get("seq")
                if ch in ("a", "b") and isinstance(seq, int):
                    got += 1
                    prev = last_seq.get(ch)
                    if prev is not None and seq <= prev:
                        order_ok = False
                        fails.append(f"phase2: fast client seq not increasing on "
                                     f"{ch!r}: {seq} after {prev}")
                    last_seq[ch] = seq
        f1 = metrics_frames_total(fast)
        fast.call("stop")
        print(f"phase2: fast client got {got} in-order frames; "
              f"frames_total {f0}->{f1} (lane serving)")
        if got < 10:
            fails.append(f"phase2: fast client only got {got} frames (stream not served)")
        if f1 <= f0:
            fails.append(f"phase2: lane not advancing for fast client ({f0}->{f1})")
        if not order_ok:
            fails.append("phase2: fast client wire order FAIL")
        fast.close(); wedged.close()
    except Exception as e:
        fails.append(f"phase2: exception: {e!r}")
    finally:
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            proc.kill()


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    fails: list[str] = []
    phase1(fails)
    phase2(fails)

    if fails:
        for f in fails:
            print("  -", f)
        print("VERDICT: FAIL: slow consumer stalls / mis-serves the ordered lane (RT8/P2)")
        return 1
    print("VERDICT: PASS: a slow-but-alive WS consumer does NOT pin the ordered lane "
          "(backend frames_total runs free, >=3x the slow drain), frames arrive in strict "
          "per-channel seq order, and a fully-wedged client is dropped within a bounded "
          "time so a fresh fast client is served an in-order stream while the lane keeps "
          "computing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
