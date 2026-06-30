"""Backend-free tests for the XEX1 decoder + the expose RunResult model.

These mirror the producer's msgpack encoding (plugins/expose/src/expose.cpp)
with a tiny local encoder, then assert the SDK decoder round-trips it. No WS
connection / backend is needed.
"""
from __future__ import annotations

import base64
import json
import math
import struct

import pytest

from xinsp2.client import (
    ExposeFrame,
    RunResult,
    MsgpackError,
    decode_xex1,
)
from xinsp2.snapshot import dump_run


# ---- a local msgpack encoder, matching the C++ producer's subset ----------

def _u16(v):
    return struct.pack(">H", v)


def _u32(v):
    return struct.pack(">I", v)


def mp_uint(v: int) -> bytes:
    if v <= 0x7F:
        return bytes([v])
    if v <= 0xFF:
        return bytes([0xCC, v])
    if v <= 0xFFFF:
        return bytes([0xCD]) + _u16(v)
    if v <= 0xFFFFFFFF:
        return bytes([0xCE]) + _u32(v)
    return bytes([0xCF]) + struct.pack(">Q", v)


def mp_str(s: str) -> bytes:
    b = s.encode("utf-8")
    n = len(b)
    if n <= 31:
        return bytes([0xA0 | n]) + b
    if n <= 0xFF:
        return bytes([0xD9, n]) + b
    if n <= 0xFFFF:
        return bytes([0xDA]) + _u16(n) + b
    return bytes([0xDB]) + _u32(n) + b


def mp_bin(b: bytes) -> bytes:
    n = len(b)
    if n <= 0xFF:
        return bytes([0xC4, n]) + b
    if n <= 0xFFFF:
        return bytes([0xC5]) + _u16(n) + b
    return bytes([0xC6]) + _u32(n) + b


def mp_arr(n: int) -> bytes:
    if n <= 15:
        return bytes([0x90 | n])
    if n <= 0xFFFF:
        return bytes([0xDC]) + _u16(n)
    return bytes([0xDD]) + _u32(n)


def mp_map(n: int) -> bytes:
    assert n <= 15
    return bytes([0x80 | n])


def build_xex1(channel: str, seq: int, values: dict, images: dict[str, bytes]) -> bytes:
    """Encode a full XEX1 frame the same way the plugin does."""
    body = mp_map(5)
    body += mp_str("v") + mp_uint(1)
    body += mp_str("channel") + mp_str(channel)
    body += mp_str("seq") + mp_uint(seq)
    body += mp_str("json") + mp_str(json.dumps(values))
    body += mp_str("images") + mp_arr(len(images))
    for key, jpeg in images.items():
        body += mp_map(2)
        body += mp_str("key") + mp_str(key)
        body += mp_str("jpeg") + mp_bin(jpeg)
    return b"XEX1" + body


# ---- decoder round-trips --------------------------------------------------

def test_decode_basic():
    frame = build_xex1("lane", 17, {"count": 4, "score": 0.5, "ok": True},
                       {"gray": b"\xff\xd8jpeg", "edges": b"img2"})
    f = decode_xex1(frame)
    assert isinstance(f, ExposeFrame)
    assert f.channel == "lane"
    assert f.seq == 17
    assert f.v == 1
    assert f.values == {"count": 4, "score": 0.5, "ok": True}
    assert f.images == {"gray": b"\xff\xd8jpeg", "edges": b"img2"}
    assert f.image("gray") == b"\xff\xd8jpeg"
    assert f.image("missing") is None


def test_decode_no_images():
    f = decode_xex1(build_xex1("c", 1, {"a": 1}, {}))
    assert f.images == {}
    assert f.values == {"a": 1}


def test_decode_large_str_and_bin():
    # exercise str16 (json>255) and bin16/bin32 paths
    big_vals = {"items": list(range(200))}          # JSON string > 255 bytes
    big_jpeg = bytes(range(256)) * 300              # > 64KiB -> bin32
    mid_jpeg = b"x" * 5000                          # > 255   -> bin16
    f = decode_xex1(build_xex1("ch", 99, big_vals, {"big": big_jpeg, "mid": mid_jpeg}))
    assert f.values == big_vals
    assert f.images["big"] == big_jpeg
    assert f.images["mid"] == mid_jpeg


def test_decode_uint_widths():
    for seq in (0, 127, 200, 70000, 5_000_000_000):
        f = decode_xex1(build_xex1("c", seq, {}, {}))
        assert f.seq == seq


def test_nonfinite_restored():
    # The producer serializes NaN/Inf as quoted sentinel strings; the decoder
    # restores them to floats (mirrors restoreNonFiniteDeep).
    body = json.dumps({"v": "NaN", "i": "Infinity", "n": "-Infinity"})
    frame = b"XEX1" + (
        mp_map(5)
        + mp_str("v") + mp_uint(1)
        + mp_str("channel") + mp_str("c")
        + mp_str("seq") + mp_uint(1)
        + mp_str("json") + mp_str(body)
        + mp_str("images") + mp_arr(0)
    )
    f = decode_xex1(frame)
    assert math.isnan(f.values["v"])
    assert f.values["i"] == math.inf
    assert f.values["n"] == -math.inf


def test_bad_magic_rejected():
    with pytest.raises(MsgpackError):
        decode_xex1(b"XPV1\x80")


def test_bad_version_rejected():
    frame = b"XEX1" + (
        mp_map(5)
        + mp_str("v") + mp_uint(2)
        + mp_str("channel") + mp_str("c")
        + mp_str("seq") + mp_uint(1)
        + mp_str("json") + mp_str("{}")
        + mp_str("images") + mp_arr(0)
    )
    with pytest.raises(MsgpackError):
        decode_xex1(frame)


def test_base64_pull_path():
    # get_expose decodes frame_b64; verify the same decoder handles it.
    raw = build_xex1("lane", 3, {"k": "v"}, {"img": b"jpeg"})
    b64 = base64.b64encode(raw).decode("ascii")
    f = decode_xex1(base64.b64decode(b64))
    assert f.channel == "lane"
    assert f.values == {"k": "v"}


# ---- RunResult + snapshot -------------------------------------------------

def test_runresult_accessors():
    f = decode_xex1(build_xex1("lane", 5, {"count": 2}, {"gray": b"jpeg-bytes"}))
    rr = RunResult(run_id=5, ms=12, frames={"lane": f})
    assert rr.expose("lane") is f
    assert rr.expose("nope") is None
    assert rr.values("lane") == {"count": 2}
    assert rr.image("lane", "gray") == b"jpeg-bytes"
    assert rr.image("lane", "nope") is None
    assert rr.image("nope", "gray") is None


def test_dump_run(tmp_path):
    f = decode_xex1(build_xex1("lane", 7, {"count": 3}, {"gray": b"\xff\xd8jpeg"}))
    rr = RunResult(run_id=7, ms=9, frames={"lane": f}, events=[])
    snap = dump_run(rr, tmp_path)
    assert snap.folder.name == "run-000007"
    report = json.loads(snap.report_path.read_text())
    assert report["run_id"] == 7
    assert report["channels"]["lane"]["values"] == {"count": 3}
    img_rel = report["channels"]["lane"]["images"][0]["file"]
    assert (snap.folder / img_rel).read_bytes() == b"\xff\xd8jpeg"
    assert (snap.folder / "lane" / "values.json").exists()
