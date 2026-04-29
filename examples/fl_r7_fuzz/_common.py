"""Shared helpers for FL r7 fuzz harnesses.

- Spawns the backend (xinsp-backend.exe) if not already running on
  ws://127.0.0.1:7823/ and tears it down on exit.
- Provides a raw WS sender that can ship arbitrary text/binary frames
  bypassing the SDK's `Client.call()` wrapper (which would JSON-encode
  for us — useless for fuzzing the parser).
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
BACKEND_EXE = REPO_ROOT / "backend" / "build" / "Release" / "xinsp-backend.exe"
WS_URL = "ws://127.0.0.1:7823/"
WS_HOST = "127.0.0.1"
WS_PORT = 7823


def port_open(host: str = WS_HOST, port: int = WS_PORT, timeout: float = 0.25) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


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
            raise FileNotFoundError(f"backend exe not found: {BACKEND_EXE}")
        print(f"[backend] spawning {BACKEND_EXE}", flush=True)
        # Detach stdio to keep our terminal clean; capture pid
        self.proc = subprocess.Popen(
            [str(BACKEND_EXE)],
            cwd=str(BACKEND_EXE.parent),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
        )
        self.spawned = True
        # wait for port
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
                # First try gentle shutdown via WS
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
        """Check just the process, not the port. None if not spawned by us."""
        if not self.spawned or self.proc is None:
            return True  # we don't own it; assume alive
        return self.proc.poll() is None

    def returncode(self):
        if self.proc is None:
            return None
        return self.proc.poll()


def open_ws(timeout: float = 5.0):
    import websocket
    return websocket.create_connection(WS_URL, timeout=timeout)
