"""xex1.py — minimal XEX1 / expose-frame decoder for example drivers.

The xinsp2 Python SDK is deliberately GENERIC and carries no plugin-specific
decoding. The `expose` plugin frames script output as a self-describing binary
WS frame; an external consumer (like these example drivers) decodes it itself.
This module is that decoder — copy-paste-free, importable by any example:

    import sys; from pathlib import Path
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
    from xex1 import decode_xex1, collect_frames, pull_latest

Frame layout (see plugins/expose/src/expose.cpp):
    [0..3] ASCII 'XEX1' | then a minimal msgpack map:
      { v:1, channel:<str>, seq:<uint>, json:<str>, images:[ {key, jpeg:<bin>} ] }
`json` is the record's values serialized to a JSON string.
"""
from __future__ import annotations

import base64
import json
import math
import struct
from typing import Any

MAGIC = b"XEX1"


class Xex1Error(ValueError):
    pass


# ---- minimal msgpack decoder (only the subset the expose frame emits) --------
def _mp(data: bytes, p: int) -> tuple[Any, int]:
    b = data[p]; p += 1
    if b <= 0x7F:                      # positive fixint
        return b, p
    if b >= 0xE0:                      # negative fixint
        return b - 0x100, p
    if 0xA0 <= b <= 0xBF:              # fixstr
        n = b & 0x1F; return data[p:p + n].decode("utf-8", "replace"), p + n
    if 0x80 <= b <= 0x8F:              # fixmap
        return _mp_map(data, p, b & 0x0F)
    if 0x90 <= b <= 0x9F:             # fixarray
        return _mp_arr(data, p, b & 0x0F)
    if b == 0xC0:  return None, p
    if b == 0xC2:  return False, p
    if b == 0xC3:  return True, p
    if b == 0xCA:  return struct.unpack_from(">f", data, p)[0], p + 4
    if b == 0xCB:  return struct.unpack_from(">d", data, p)[0], p + 8  # float64 (pack plane)
    if b == 0xCC:  return data[p], p + 1
    if b == 0xCD:  return struct.unpack_from(">H", data, p)[0], p + 2
    if b == 0xCE:  return struct.unpack_from(">I", data, p)[0], p + 4
    if b == 0xCF:  return struct.unpack_from(">Q", data, p)[0], p + 8
    if b == 0xD0:  return struct.unpack_from(">b", data, p)[0], p + 1
    if b == 0xD1:  return struct.unpack_from(">h", data, p)[0], p + 2
    if b == 0xD2:  return struct.unpack_from(">i", data, p)[0], p + 4
    if b == 0xD3:  return struct.unpack_from(">q", data, p)[0], p + 8  # int64 (pack plane)
    if b == 0xD9:  n = data[p]; p += 1; return data[p:p + n].decode("utf-8", "replace"), p + n
    if b == 0xDA:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return data[p:p + n].decode("utf-8", "replace"), p + n
    if b == 0xDB:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return data[p:p + n].decode("utf-8", "replace"), p + n
    if b == 0xC4:  n = data[p]; p += 1; return data[p:p + n], p + n
    if b == 0xC5:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return data[p:p + n], p + n
    if b == 0xC6:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return data[p:p + n], p + n
    if b == 0xDC:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return _mp_arr(data, p, n)
    if b == 0xDD:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return _mp_arr(data, p, n)
    if b == 0xDE:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return _mp_map(data, p, n)  # map16 (>15 keys)
    if b == 0xDF:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return _mp_map(data, p, n)  # map32
    raise Xex1Error(f"unsupported msgpack byte 0x{b:02x}")


def _mp_map(data: bytes, p: int, n: int) -> tuple[dict, int]:
    o: dict = {}
    for _ in range(n):
        k, p = _mp(data, p)
        v, p = _mp(data, p)
        o[k] = v
    return o, p


def _mp_arr(data: bytes, p: int, n: int) -> tuple[list, int]:
    a: list = []
    for _ in range(n):
        v, p = _mp(data, p)
        a.append(v)
    return a, p


_NONFINITE = {"NaN": math.nan, "Infinity": math.inf, "-Infinity": -math.inf}


def _restore_nonfinite(v: Any) -> Any:
    if isinstance(v, str):
        return _NONFINITE.get(v, v)
    if isinstance(v, list):
        return [_restore_nonfinite(x) for x in v]
    if isinstance(v, dict):
        return {k: _restore_nonfinite(x) for k, x in v.items()}
    return v


# The pack-plane entry tag vocabulary (XI_PACK_TAG_* in xi_abi.h). v3 carries
# every frame entry as [tag, value], so an image descriptor is identified by
# ITS TAG — never by shape (the v2 draft's ambiguity fix).
TAG_IMAGE = 4


def jpeg_dims(data: bytes) -> tuple[int, int] | None:
    """Read (width, height) from a JPEG's first SOF marker — a dependency-free
    way to prove a `preview` entry is FULL resolution (its decoded dims match the
    source), without pulling in Pillow. Returns None if not a parseable JPEG."""
    if len(data) < 2 or data[0] != 0xFF or data[1] != 0xD8:  # SOI
        return None
    i = 2
    n = len(data)
    while i + 3 < n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        i += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:  # standalone markers
            continue
        if i + 1 >= n:
            break
        seg_len = (data[i] << 8) | data[i + 1]
        # SOF0..SOF3, SOF5..SOF7, SOF9..SOF11, SOF13..SOF15 carry the frame dims.
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if i + 4 < n:
                h = (data[i + 3] << 8) | data[i + 4]
                w = (data[i + 5] << 8) | data[i + 6]
                return (w, h)
            return None
        i += seg_len
    return None


def _decode_xex1_v3(body: dict) -> dict:
    """Normalize an XEX1-v3 canonical frame dump {v:3, channel, seq,
    frame:{key: [tag, value], ...}} into {v, channel, seq, values, images}.
    tag 4 (image descriptor {w,h,c,px}) -> images[key] = {w,h,c,pixels}; every
    other tag -> values. v3 is lossless (raw pixels as msgpack bin), no JPEG.

    E2: an image descriptor MAY additionally carry a nested `preview` map
    { w,h,c, enc:"jpeg", q, data:<jpeg bytes> } — expose's WS-SEND-only
    full-resolution compressed substitution (the raw `px` is then empty on the
    wire). When present it surfaces as images[key]["preview"]; the raw-pixel
    fields stay for decoders that ignore it (record_save/disk always ship raw)."""
    frame = body.get("frame") or {}
    values: dict = {}
    images: dict = {}
    for k, e in frame.items():
        if not (isinstance(e, list) and len(e) == 2):
            continue   # not a [tag, value] pair
        tag, v = e
        if tag == TAG_IMAGE and isinstance(v, dict):
            entry = {"w": v.get("w"), "h": v.get("h"), "c": v.get("c"),
                     "pixels": bytes(v.get("px") or b"")}
            pv = v.get("preview")
            if isinstance(pv, dict) and pv.get("data") is not None:
                entry["preview"] = {
                    "w": pv.get("w"), "h": pv.get("h"), "c": pv.get("c"),
                    "enc": pv.get("enc"), "q": pv.get("q"),
                    "data": bytes(pv.get("data") or b""),
                }
            images[k] = entry
        else:
            values[k] = v
    return {"v": 3, "channel": body.get("channel"), "seq": body.get("seq"),
            "values": values, "images": images, "frame": frame}


def decode_xex1(data: bytes) -> dict:
    """Decode an XEX1 frame. v1 -> {v, channel, seq, values, images:dict[key]->jpeg
    bytes}; v3 (the canonical frame dump, per-entry tags) -> {v, channel, seq,
    values, images:dict[key]->{w,h,c,pixels}}. Raises Xex1Error on a bad magic /
    unsupported version (including the retired tagless v2 draft)."""
    if len(data) < 5 or data[:4] != MAGIC:
        raise Xex1Error("not an XEX1 frame")
    body, _ = _mp(data, 4)
    if not isinstance(body, dict):
        raise Xex1Error("XEX1 body is not a msgpack map")
    v = body.get("v")
    if v == 3:
        return _decode_xex1_v3(body)
    if v != 1:
        raise Xex1Error(f"unsupported XEX1 version {v}")
    values = {}
    try:
        values = _restore_nonfinite(json.loads(body.get("json") or "{}"))
    except Exception:
        values = {}
    images = {img["key"]: img["jpeg"] for img in (body.get("images") or [])
              if isinstance(img, dict) and "key" in img}
    return {"v": 1, "channel": body.get("channel"), "seq": body.get("seq"),
            "values": values, "images": images}


def is_xex1(data: bytes) -> bool:
    return len(data) >= 4 and data[:4] == MAGIC


# ---- driver helpers ----------------------------------------------------------
def subscribe(client, channels, inst: str = "expose") -> None:
    client.exchange_instance(inst, {"command": "subscribe", "channels": list(channels)})


def unsubscribe(client, channels, inst: str = "expose") -> None:
    client.exchange_instance(inst, {"command": "unsubscribe", "channels": list(channels)})


def list_channels(client, inst: str = "expose") -> dict:
    return client.exchange_instance(inst, {"command": "list_channels"})


def pull_latest(client, channel: str, inst: str = "expose") -> dict | None:
    """Pull the latest frame for a channel via exchange `get` (works without the
    live binary stream). Returns the decoded frame or None."""
    r = client.exchange_instance(inst, {"command": "get", "channel": channel})
    if not r or not r.get("found") or not r.get("frame_b64"):
        return None
    return decode_xex1(base64.b64decode(r["frame_b64"]))


def collect_frames(client) -> list[dict]:
    """Drain the client's raw binary inbox and decode every XEX1 frame queued
    since the last drain. Requires the client to have surfaced binary frames
    (Client._inbox_binary). Non-XEX1 frames are skipped."""
    out: list[dict] = []
    for raw in client.drain_binary():
        if is_xex1(raw):
            try:
                out.append(decode_xex1(raw))
            except Xex1Error:
                pass
    return out
