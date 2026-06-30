"""Shared helpers for the salvaged r7/r8 fuzz harnesses.

Provenance
----------
Merged + de-drifted from the unmerged survey branches
`origin/feature/fl-r7-fuzz` (`examples/fl_r7_fuzz/_common.py`) and
`origin/feature/fl-r8-concurrency-fuzz` (`examples/fl_r8_concurrency/_common.py`).
See README.md for the salvage rationale (core_fix_plan.md Part IV §25).

What this provides
------------------
- Spawns the backend (xinsp-backend.exe) if not already running on
  ws://127.0.0.1:7823/ and tears it down on exit (``BackendProc``).
- A raw WS sender (``open_ws``) that can ship arbitrary text/binary
  frames bypassing the SDK's JSON-encoding wrapper — needed to fuzz the
  WS cmd parser.
- Small env helpers (``fuzz_iters`` / ``fuzz_duration``) so every
  harness honours a reduced-iteration smoke mode out of the box.
- ``drain_events`` — current replacement for the removed
  ``Client.next_vars`` (the per-event VAR model was deleted from core;
  see the protocol-drift note in README.md).
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import time
from pathlib import Path

# tests/fuzz/_common.py -> parents[0]=fuzz, [1]=tests, [2]=repo root
REPO_ROOT = Path(__file__).resolve().parents[2]
WS_HOST = "127.0.0.1"
# The WS server is single-client. On a dev box the VS Code extension
# auto-grabs the slot on the default port 7823 the instant a backend
# opens it, which races (and 503s) a fuzz client. Set XINSP_WS_PORT to a
# free port (e.g. 7824) to run the harnesses on a backend the extension
# is not watching. We pass --port to the spawned backend to match.
WS_PORT = int(os.environ.get("XINSP_WS_PORT", "7823"))
WS_URL = f"ws://{WS_HOST}:{WS_PORT}/"


def _find_backend_exe() -> Path:
    """Locate the backend exe produced by the normal build.

    Prefer Release, fall back to Debug. An explicit override via
    ``XINSP_BACKEND_EXE`` wins (handy for CI / non-default build dirs).
    """
    override = os.environ.get("XINSP_BACKEND_EXE")
    if override:
        return Path(override)
    base = REPO_ROOT / "backend" / "build"
    for cfg in ("Release", "Debug"):
        cand = base / cfg / "xinsp-backend.exe"
        if cand.exists():
            return cand
    # last resort: anything under the build tree
    for cand in base.rglob("xinsp-backend.exe"):
        return cand
    # default path (will raise a clear FileNotFoundError at spawn time)
    return base / "Release" / "xinsp-backend.exe"


BACKEND_EXE = _find_backend_exe()


def fuzz_iters(default: int) -> int:
    """Iteration count, overridable via ``FUZZ_ITERS`` (smoke mode)."""
    return int(os.environ.get("FUZZ_ITERS", str(default)))


def fuzz_duration(default: float) -> float:
    """Wall-clock budget in seconds, overridable via ``FUZZ_DURATION``."""
    return float(os.environ.get("FUZZ_DURATION", str(default)))


def port_open(host: str = WS_HOST, port: int = WS_PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_port_free(timeout: float = 10.0) -> bool:
    """Block until nothing is listening on the WS port.

    The server is single-client; a backend that is still draining from a
    previous harness will otherwise get attached-to and reject our
    connect with HTTP 503 ``single-client-busy``. Call this between
    harness runs that each spawn their own backend.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not port_open():
            return True
        time.sleep(0.25)
    return not port_open()


class BackendProc:
    """Context manager: own-the-backend if we spawned it, leave alone if not."""

    def __init__(self):
        self.proc: subprocess.Popen | None = None
        self.spawned = False

    def __enter__(self):
        if port_open():
            print(f"[backend] already running on :{WS_PORT}, attaching", flush=True)
            return self
        if not BACKEND_EXE.exists():
            raise FileNotFoundError(
                f"backend exe not found: {BACKEND_EXE}\n"
                f"Build it with the normal backend build, or set "
                f"XINSP_BACKEND_EXE to its path."
            )
        print(f"[backend] spawning {BACKEND_EXE} --port={WS_PORT}", flush=True)
        self.proc = subprocess.Popen(
            [str(BACKEND_EXE), f"--port={WS_PORT}"],
            cwd=str(BACKEND_EXE.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        self.spawned = True
        deadline = time.time() + 15.0
        while time.time() < deadline:
            if port_open():
                print(f"[backend] up (pid={self.proc.pid})", flush=True)
                return self
            if self.proc.poll() is not None:
                raise RuntimeError(f"backend exited prematurely rc={self.proc.returncode}")
            time.sleep(0.1)
        raise TimeoutError("backend did not open WS port within 15s")

    def __exit__(self, *exc):
        if self.spawned and self.proc and self.proc.poll() is None:
            print(f"[backend] terminating pid={self.proc.pid}", flush=True)
            try:
                import websocket
                ws = websocket.create_connection(WS_URL, timeout=2.0)
                ws.send(json.dumps({"type": "cmd", "id": 999999, "name": "shutdown"}))
                ws.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    self.proc.kill()

    def alive(self) -> bool:
        if not self.spawned:
            return port_open()
        return self.proc is not None and self.proc.poll() is None and port_open()

    def proc_alive(self) -> bool:
        """Check just the process, not the port. True if we don't own it."""
        if not self.spawned or self.proc is None:
            return True
        return self.proc.poll() is None

    def returncode(self):
        if self.proc is None:
            return None
        return self.proc.poll()


def open_ws(timeout: float = 5.0):
    import websocket
    return websocket.create_connection(WS_URL, timeout=timeout)


def drain_events(client) -> int:
    """Drain whatever the SDK client has queued (events + binary frames).

    Replacement for the removed ``Client.next_vars``: the per-event VAR
    model was deleted from core (VAR became the ``expose`` plugin), so
    there is no vars feed to pop. Harnesses only ever used the drain to
    avoid unbounded inbox growth under load; counting drained frames is
    a good-enough liveness signal. Returns the number of items drained.
    """
    n = 0
    try:
        n += len(client.drain_binary())
    except Exception:
        pass
    # Best-effort: empty the events queue if the SDK exposes it.
    q = getattr(client, "_inbox_events", None)
    if q is not None:
        from queue import Empty
        while True:
            try:
                q.get_nowait()
                n += 1
            except Empty:
                break
            except Exception:
                break
    return n
