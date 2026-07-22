"""Client-side auth flow construction — the handshake headers the SDK emits for
each mode, verified against an independent reference (no live backend needed).

The header shapes here mirror the server verification in
`backend/include/xi/xi_ws_server.hpp`. `test_auth_live.py` proves the same
headers are actually accepted/refused by a real backend; this file pins the
exact bytes so a regression is caught even where the C++ build is unavailable.

    pytest tools/xinsp2_py/tests/test_auth_flow.py
"""
from __future__ import annotations

import hashlib
import hmac

import pytest

from xinsp2 import Client


def test_bearer_headers():
    c = Client(token="s3cr3t", auth_mode="bearer")
    h = c._auth_headers()
    assert h == {"Authorization": "Bearer s3cr3t"}
    assert "X-Xi-Timestamp" not in h


def test_hmac_headers_match_reference():
    key = "hmac-shared-key"
    c = Client(token=key, auth_mode="hmac")
    h = c._auth_headers()
    # Timestamp is a plain unix-seconds decimal string.
    ts = h["X-Xi-Timestamp"]
    assert ts.isdigit()
    # The signed message is exactly that timestamp string; digest is lowercase
    # hex — recompute independently and compare.
    expected = hmac.new(key.encode(), ts.encode(), hashlib.sha256).hexdigest()
    assert h["Authorization"] == f"Bearer {expected}"
    assert expected == expected.lower() and len(expected) == 64


def test_hmac_key_never_appears_on_the_wire():
    # The raw key must not leak into any header value in hmac mode — that is
    # the whole point of the mode over plain bearer.
    key = "super-secret-key-abc123"
    h = Client(token=key, auth_mode="hmac")._auth_headers()
    for v in h.values():
        assert key not in v


def test_no_token_means_no_auth_headers():
    assert Client()._auth_headers() == {}
    assert Client(token=None, auth_mode="hmac")._auth_headers() == {}


def test_invalid_auth_mode_rejected():
    with pytest.raises(ValueError):
        Client(token="x", auth_mode="challenge")
