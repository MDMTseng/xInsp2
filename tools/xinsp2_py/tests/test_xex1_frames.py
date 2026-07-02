"""Cross-implementation test of the XEX1 binary frame decoder against the C++
encoder's golden frames.

The XEX1 frame is encoded in C++ (plugins/expose/src/xex1_encode.hpp) and decoded,
independently, by the expose plugin's webUI (plugins/expose/ui/index.html) and by
examples/lib/xex1.py — the decoder under test here. This loads every golden .bin
under protocol/fixtures/binary/ and asserts the decoded content matches the
committed manifest, the same manifest the JS test (ui-components/test/xex1-golden.mjs)
checks. Regenerate the goldens with the expose plugin's `xex1_fixtures_test --regen`.

    pytest tools/xinsp2_py/tests/test_xex1_frames.py

Runs without a live backend or the xinsp2 package — it imports the standalone
decoder from examples/lib/xex1.py directly.
"""
from __future__ import annotations

import base64
import json
import math
import sys
from pathlib import Path

import pytest

# repo root is four levels up: tools/xinsp2_py/tests/test_xex1_frames.py -> repo root
REPO = Path(__file__).resolve().parents[3]
FIX = REPO / "protocol" / "fixtures" / "binary"
sys.path.insert(0, str(REPO / "examples" / "lib"))

from xex1 import decode_xex1, is_xex1, _mp  # noqa: E402

MANIFEST = json.loads((FIX / "manifest.json").read_text(encoding="utf-8"))
FRAMES = MANIFEST["frames"]


def _raw_body(data: bytes) -> dict:
    """Decode at the msgpack level (past the 4-byte magic) to inspect the frame's
    literal fields — the `json` string before JSON parsing, and any extension keys."""
    body, _ = _mp(data, 4)
    return body


@pytest.mark.parametrize("frame", FRAMES, ids=[f["file"] for f in FRAMES])
def test_golden_frame_decodes_to_manifest(frame: dict) -> None:
    data = (FIX / frame["file"]).read_bytes()
    assert is_xex1(data)

    if frame.get("v") == 2:
        _check_v2(frame, data)
        return

    raw = _raw_body(data)
    assert raw["v"] == 1
    assert raw["channel"] == frame["channel"]
    assert raw["seq"] == frame["seq"]
    assert raw["json"] == frame["json"]
    for k in frame["extra_keys"]:
        assert k in raw, f"missing extension key {k}"

    # High-level decode: channel/seq + byte-exact images keyed by name.
    dec = decode_xex1(data)
    assert dec["channel"] == frame["channel"]
    assert dec["seq"] == frame["seq"]
    assert list(dec["images"].keys()) == [im["key"] for im in frame["images"]]
    for im in frame["images"]:
        assert dec["images"][im["key"]] == base64.b64decode(im["b64"])
        assert len(dec["images"][im["key"]]) == im["size"]


def _check_v2(frame: dict, data: bytes) -> None:
    """The canonical frame dump: v:2 with scalar/str/mp entries -> values and
    image descriptors -> images (raw pixels, lossless)."""
    dec = decode_xex1(data)
    assert dec["v"] == 2
    assert dec["channel"] == frame["channel"]
    assert dec["seq"] == frame["seq"]
    for fld in frame["fields"]:
        key, kind = fld["key"], fld["kind"]
        if kind == "image":
            im = dec["images"][key]
            assert (im["w"], im["h"], im["c"]) == (fld["w"], fld["h"], fld["c"])
            assert im["pixels"] == base64.b64decode(fld["b64"])
        elif kind == "bin":
            assert bytes(dec["values"][key]) == base64.b64decode(fld["b64"])
        else:
            assert dec["values"][key] == fld["value"]


def test_nonfinite_sentinels_restore() -> None:
    dec = decode_xex1((FIX / "nonfinite.bin").read_bytes())
    v = dec["values"]
    assert math.isnan(v["a"])
    assert math.isinf(v["b"]) and v["b"] > 0
    assert math.isinf(v["c"]) and v["c"] < 0
    assert v["d"] == 1.5


def test_map16_frame_crosses_the_fixmap_cap() -> None:
    """The map16 golden has >15 top-level keys — the frame a fixmap-only decoder
    could not read (it would raise on the 0xDE opcode). Confirm the golden really
    uses map16 and that the widened decoder reads all its keys."""
    data = (FIX / "map16.bin").read_bytes()
    assert data[4] == 0xDE, "map16.bin must use the map16 opcode after the magic"
    raw = _raw_body(data)
    assert len(raw) == 17  # 5 canonical + 12 extension keys
    assert all(f"ext{i:02d}" in raw for i in range(12))
    # Before the fix, _mp had no 0xDE branch and hit its final `raise Xex1Error
    # ("unsupported msgpack byte 0xde")`; the decode above succeeding is the fix.
    assert decode_xex1(data)["channel"] == "c"
