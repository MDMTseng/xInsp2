"""End-to-end auth against a REAL backend spawned with `--auth`.

Spawns `backend/build/Release/xinsp-backend.exe` on an ephemeral port with a
shared secret and drives the SDK's `connect()` through both handshake modes:

- bearer: right token connects + `ping` works; wrong token and missing token
  are refused with a clean `AuthError` (HTTP 401), NOT a hang;
- hmac: right key connects + `ping` works; wrong key is refused; a plain-bearer
  attempt against an hmac server is refused (confirming the modes don't mix);
- no-auth: a backend started without `--auth` accepts an unauthenticated client.

The whole suite auto-skips if the backend binary isn't built, so it stays green
in environments without the C++ toolchain while exercising the real wire where
the build exists. The backend enables auth via `--auth=<secret>` (plain bearer)
or `--auth=hmac:<key>` (hmac); the `XINSP2_AUTH` env var is the ps-safe
equivalent (`backend/include/xi/xi_cli_args.hpp`).

    pytest tools/xinsp2_py/tests/test_auth_live.py
"""
from __future__ import annotations

import contextlib
import socket
import subprocess
import time
from pathlib import Path

import pytest

from xinsp2 import Client, AuthError

BACKEND = (
    Path(__file__).resolve().parents[3]
    / "backend" / "build" / "Release" / "xinsp-backend.exe"
)

pytestmark = pytest.mark.skipif(
    not BACKEND.exists(),
    reason=f"backend binary not built at {BACKEND}",
)


def _free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


@contextlib.contextmanager
def spawn_backend(*extra_args: str):
    """Start a backend on a free loopback port; yield its ws:// url."""
    port = _free_port()
    proc = subprocess.Popen(
        [str(BACKEND), f"--port={port}", "--host=127.0.0.1", *extra_args],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        deadline = time.time() + 20
        while time.time() < deadline:
            if proc.poll() is not None:
                out = proc.stdout.read() if proc.stdout else ""
                raise RuntimeError(f"backend exited early ({proc.returncode}): {out}")
            with contextlib.suppress(OSError):
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    break
            time.sleep(0.1)
        else:
            raise RuntimeError("backend did not open its port in time")
        yield f"ws://127.0.0.1:{port}/"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


SECRET = "live-bearer-secret-9f3c"
HMAC_KEY = "live-hmac-key-1a2b3c"


# ---- bearer -----------------------------------------------------------------

def test_bearer_correct_token_connects_and_pings():
    with spawn_backend(f"--auth={SECRET}") as url:
        c = Client(url, timeout=5, token=SECRET, auth_mode="bearer")
        hello = c.connect()
        try:
            assert hello.get("version")
            assert c.ping().get("pong") is True
        finally:
            c.close()


def test_bearer_wrong_token_is_clean_autherror_not_hang():
    with spawn_backend(f"--auth={SECRET}") as url:
        c = Client(url, timeout=5, token="WRONG-TOKEN", auth_mode="bearer")
        t0 = time.time()
        with pytest.raises(AuthError) as ei:
            c.connect()
        # A refusal, not a stall: the 401 comes back well inside the timeout.
        assert time.time() - t0 < 5
        assert ei.value.status_code == 401
        assert ei.value.mode == "bearer"
        assert isinstance(ei.value, ConnectionError)


def test_missing_token_against_authed_backend_is_refused():
    with spawn_backend(f"--auth={SECRET}") as url:
        c = Client(url, timeout=5)  # no token
        with pytest.raises(AuthError):
            c.connect()


# ---- hmac -------------------------------------------------------------------

def test_hmac_correct_key_connects_and_pings():
    with spawn_backend(f"--auth=hmac:{HMAC_KEY}") as url:
        c = Client(url, timeout=5, token=HMAC_KEY, auth_mode="hmac")
        hello = c.connect()
        try:
            assert hello.get("version")
            assert c.ping().get("pong") is True
        finally:
            c.close()


def test_hmac_wrong_key_is_refused():
    with spawn_backend(f"--auth=hmac:{HMAC_KEY}") as url:
        c = Client(url, timeout=5, token="WRONG-KEY", auth_mode="hmac")
        with pytest.raises(AuthError) as ei:
            c.connect()
        assert ei.value.status_code == 401


def test_plain_bearer_against_hmac_server_is_refused():
    # Sending the key as a plain bearer to an hmac server must fail — this is
    # why the SDK does not silently fall back between modes (it would leak the
    # key on the wire).
    with spawn_backend(f"--auth=hmac:{HMAC_KEY}") as url:
        c = Client(url, timeout=5, token=HMAC_KEY, auth_mode="bearer")
        with pytest.raises(AuthError):
            c.connect()


# ---- no auth ----------------------------------------------------------------

def test_no_auth_backend_accepts_unauthenticated_client():
    with spawn_backend() as url:
        c = Client(url, timeout=5)
        hello = c.connect()
        try:
            assert hello.get("version")
        finally:
            c.close()
