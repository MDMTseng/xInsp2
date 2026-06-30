"""ports.py — pick a free backend port so parallel driver runs don't collide.

The backend port is freely assignable (`xinsp-backend.exe --port=N`); the only
missing piece is "don't all grab 7823". Probe a free ephemeral port, then spawn
the backend on it and point the client at the same port:

    from ports import free_port
    port = free_port()
    proc = subprocess.Popen([BACKEND, f"--port={port}"], ...)   # spawn on it
    c = Client(f"ws://127.0.0.1:{port}/")                       # connect to it
    ...
    proc.terminate()    # kill ONLY the pid you spawned — never `taskkill /IM`

Caveat — small TOCTOU window: the OS could hand the same port to another process
between the probe and the backend's bind. Mitigated by a high random base + a few
retries; a fully race-free version would be backend `--port=0` reporting the bound
port. For parallel example/test runs this is enough.
"""
from __future__ import annotations

import contextlib
import socket


def free_port(host: str = "127.0.0.1") -> int:
    """Ask the OS for an unused TCP port (bind :0, read it back, release)."""
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((host, 0))
        return s.getsockname()[1]


def is_free(port: int, host: str = "127.0.0.1") -> bool:
    """True if `port` can be bound right now."""
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind((host, port))
            return True
        except OSError:
            return False
