"""Tests for the canonical max-width msgpack profile (xinsp2.canonical).

Covers: determinism, the int64/uint64 boundaries proving no compaction ever
happens, float canonicalization (NaN pattern, -0.0, inf), hostile-input
rejection (truncation, depth bombs, trailing bytes, disallowed ext), the
compact-in -> canonical-out ingress pass, and the SHARED cross-language vectors
in contract/canonical-vectors.json (the same file the TypeScript suite checks —
their agreement is the cross-language validation of the profile).

    pytest tools/xinsp2_py/tests/test_canonical.py
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import pytest

from xinsp2.canonical import (
    Ext,
    DecodeError,
    DepthLimitError,
    EncodeError,
    ExtNotAllowedError,
    IntRangeError,
    MapKeyError,
    TrailingBytesError,
    TruncatedError,
    INT64_MAX,
    INT64_MIN,
    UINT64_MAX,
    decode,
    encode_canonical,
    recanonicalize,
)

REPO = Path(__file__).resolve().parents[3]
VECTORS = json.loads((REPO / "contract" / "canonical-vectors.json").read_text(encoding="utf-8"))


# ---- profile invariants: everything is max width ---------------------------

def test_int_always_int64_marker():
    for v in [0, 1, -1, 127, 128, 255, 256, 65535, 65536, INT64_MAX, INT64_MIN]:
        out = encode_canonical(v)
        assert len(out) == 9, f"{v} should be 1 marker + 8 bytes"
        assert out[0] == 0xD3, f"{v} must use int64 marker, got 0x{out[0]:02x}"


def test_int_beyond_int64_uses_uint64():
    for v in [INT64_MAX + 1, UINT64_MAX]:
        out = encode_canonical(v)
        assert out[0] == 0xCF
        assert len(out) == 9


def test_int_out_of_range_rejected():
    with pytest.raises(IntRangeError):
        encode_canonical(UINT64_MAX + 1)
    with pytest.raises(IntRangeError):
        encode_canonical(INT64_MIN - 1)


def test_float_always_float64():
    for v in [0.0, 1.0, 0.5, -2.5, 1e308]:
        out = encode_canonical(v)
        assert out[0] == 0xCB
        assert len(out) == 9


def test_str_always_str32():
    out = encode_canonical("hi")
    assert out[0] == 0xDB
    assert out[1:5] == struct.pack(">I", 2)


def test_bin_always_bin32():
    out = encode_canonical(b"\x01\x02")
    assert out[0] == 0xC6
    assert out[1:5] == struct.pack(">I", 2)


def test_container_always_widest():
    assert encode_canonical([1, 2])[0] == 0xDD          # array32
    assert encode_canonical({"a": 1})[0] == 0xDF        # map32


# ---- determinism -----------------------------------------------------------

def test_encode_is_deterministic():
    obj = {"b": 2, "a": [1, 2, {"x": True}], "c": "str"}
    assert encode_canonical(obj) == encode_canonical(obj)


def test_map_key_order_is_insertion_order():
    # Insertion order is the caller's contract, not sorted.
    a = encode_canonical({"z": 1, "a": 2})
    b = encode_canonical({"a": 2, "z": 1})
    assert a != b, "different insertion order must produce different bytes"


def test_bool_not_confused_with_int():
    assert encode_canonical(True) == b"\xc3"
    assert encode_canonical(False) == b"\xc2"
    assert encode_canonical(1) != encode_canonical(True)


# ---- float canonicalization -------------------------------------------------

def test_nan_normalized_to_canonical_pattern():
    canonical = bytes.fromhex("cb7ff8000000000000")
    for bits in [0x7FF8000000000000, 0xFFF8000000000001, 0x7FF0000000000001]:
        v = struct.unpack(">d", struct.pack(">Q", bits))[0]
        assert encode_canonical(v) == canonical


def test_negative_zero_preserved():
    assert encode_canonical(-0.0) == bytes.fromhex("cb8000000000000000")
    assert encode_canonical(0.0) == bytes.fromhex("cb0000000000000000")


def test_infinities_preserved():
    assert encode_canonical(float("inf")) == bytes.fromhex("cb7ff0000000000000")
    assert encode_canonical(float("-inf")) == bytes.fromhex("cbfff0000000000000")


# ---- round trips ------------------------------------------------------------

@pytest.mark.parametrize("value", [
    None, True, False, 0, -1, 42, INT64_MAX, INT64_MIN, UINT64_MAX,
    1.5, -0.0, "", "héllo", b"", b"\x00\xff",
    [], [1, "a", None], {}, {"k": "v", "n": [1, 2]},
])
def test_round_trip(value):
    decoded = decode(encode_canonical(value))
    if isinstance(value, float):
        assert struct.pack(">d", decoded) == struct.pack(">d", value)
    else:
        assert decoded == value


def test_uint64_above_int64_round_trips():
    v = INT64_MAX + 100
    assert decode(encode_canonical(v)) == v


# ---- decoder accepts compact / standard forms ------------------------------

def test_decode_reads_compact_forms():
    assert decode(b"\x05") == 5                       # fixint
    assert decode(b"\xfb") == -5                      # negative fixint
    assert decode(bytes.fromhex("ccc8")) == 200       # uint8
    assert decode(bytes.fromhex("ca3fc00000")) == 1.5  # float32 widened to double
    assert decode(bytes.fromhex("a568656c6c6f")) == "hello"  # fixstr


# ---- hostile inputs are rejected -------------------------------------------

def test_truncated_scalar_rejected():
    with pytest.raises(TruncatedError):
        decode(bytes.fromhex("d300000000"))          # int64 marker, only 4 payload bytes


def test_truncated_str_length_rejected():
    # str32 claiming 1000 bytes but supplying none.
    with pytest.raises(TruncatedError):
        decode(bytes.fromhex("db000003e8"))


def test_array_length_lie_rejected_before_alloc():
    # array32 claiming 4 billion elements in a 5-byte buffer.
    with pytest.raises(TruncatedError):
        decode(bytes.fromhex("ddffffffff"))


def test_map_length_lie_rejected_before_alloc():
    with pytest.raises(TruncatedError):
        decode(bytes.fromhex("dfffffffff"))


def test_depth_bomb_rejected():
    # deeply nested fixarrays: 200 x 0x91 then a 0. Exceeds default max_depth=64.
    payload = b"\x91" * 200 + b"\x00"
    with pytest.raises(DepthLimitError):
        decode(payload)


def test_depth_limit_configurable():
    payload = b"\x91" * 3 + b"\x00"          # 3 levels of nesting
    assert decode(payload, max_depth=64) == [[[0]]]
    with pytest.raises(DepthLimitError):
        decode(payload, max_depth=2)


def test_trailing_bytes_rejected():
    with pytest.raises(TrailingBytesError):
        decode(encode_canonical(1) + b"\x00")


def test_invalid_byte_0xc1_rejected():
    with pytest.raises(DecodeError):
        decode(b"\xc1")


def test_non_string_map_key_rejected():
    # map with an integer key: {1: 2} compact = 0x81 0x01 0x02
    with pytest.raises(MapKeyError):
        decode(bytes.fromhex("810102"))


# ---- ext handling -----------------------------------------------------------

def test_ext_rejected_by_default():
    # fixext1 type=7 data=0xaa
    with pytest.raises(ExtNotAllowedError):
        decode(bytes.fromhex("d407aa"))


def test_ext_allowed_when_whitelisted():
    got = decode(bytes.fromhex("d407aa"), allow_ext=(7,))
    assert got == Ext(7, b"\xaa")


def test_encoder_refuses_to_emit_ext():
    with pytest.raises(EncodeError):
        encode_canonical(Ext(7, b"\xaa"))


def test_recanonicalize_refuses_ext_by_default():
    with pytest.raises(ExtNotAllowedError):
        recanonicalize(bytes.fromhex("d407aa"))


def test_unsupported_type_rejected():
    with pytest.raises(EncodeError):
        encode_canonical(object())


# ---- recanonicalize (ingress) ----------------------------------------------

def test_recanonicalize_widens_compact():
    assert recanonicalize(b"\x05") == bytes.fromhex("d30000000000000005")


def test_recanonicalize_is_idempotent():
    obj = {"w": 640, "h": 480, "roi": [1, 2, 3, 4], "scale": 1.5, "ok": True}
    once = encode_canonical(obj)
    assert recanonicalize(once) == once


def test_recanonicalize_normalizes_nan():
    weird_nan = bytes.fromhex("cbfff8000000000001")
    assert recanonicalize(weird_nan) == bytes.fromhex("cb7ff8000000000000")


def test_recanonicalize_validates():
    with pytest.raises(TruncatedError):
        recanonicalize(bytes.fromhex("ddffffffff"))


# ---- shared cross-language vectors -----------------------------------------

def _build(node):
    t = node["t"]
    if t == "null":
        return None
    if t == "bool":
        return node["v"]
    if t == "int":
        return int(node["v"])
    if t == "f64":
        if "bits" in node:
            return struct.unpack(">d", bytes.fromhex(node["bits"]))[0]
        return float(node["v"])
    if t == "str":
        return node["v"]
    if t == "bin":
        return bytes.fromhex(node["hex"])
    if t == "array":
        return [_build(x) for x in node["v"]]
    if t == "map":
        return {k: _build(x) for k, x in node["v"]}
    raise ValueError(t)


@pytest.mark.parametrize("case", VECTORS["encode"], ids=[c["name"] for c in VECTORS["encode"]])
def test_shared_encode_vectors(case):
    assert encode_canonical(_build(case["value"])).hex() == case["hex"]


@pytest.mark.parametrize("case", VECTORS["recanon"], ids=[c["name"] for c in VECTORS["recanon"]])
def test_shared_recanon_vectors(case):
    assert recanonicalize(bytes.fromhex(case["in_hex"])).hex() == case["out_hex"]
