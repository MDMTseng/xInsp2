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
    if b == 0xCC:  return data[p], p + 1
    if b == 0xCD:  return struct.unpack_from(">H", data, p)[0], p + 2
    if b == 0xCE:  return struct.unpack_from(">I", data, p)[0], p + 4
    if b == 0xCF:  return struct.unpack_from(">Q", data, p)[0], p + 8
    if b == 0xD9:  n = data[p]; p += 1; return data[p:p + n].decode("utf-8", "replace"), p + n
    if b == 0xDA:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return data[p:p + n].decode("utf-8", "replace"), p + n
    if b == 0xDB:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return data[p:p + n].decode("utf-8", "replace"), p + n
    if b == 0xC4:  n = data[p]; p += 1; return data[p:p + n], p + n
    if b == 0xC5:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return data[p:p + n], p + n
    if b == 0xC6:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return data[p:p + n], p + n
    if b == 0xDC:  n = struct.unpack_from(">H", data, p)[0]; p += 2; return _mp_arr(data, p, n)
    if b == 0xDD:  n = struct.unpack_from(">I", data, p)[0]; p += 4; return _mp_arr(data, p, n)
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


def decode_xex1(data: bytes) -> dict:
    """Decode an XEX1 frame -> {v, channel, seq, values:dict, images:dict[key]->jpeg bytes}.
    Raises Xex1Error on a bad magic / unsupported version."""
    if len(data) < 5 or data[:4] != MAGIC:
        raise Xex1Error("not an XEX1 frame")
    body, _ = _mp(data, 4)
    if not isinstance(body, dict) or body.get("v") != 1:
        raise Xex1Error(f"unsupported XEX1 version {body.get('v') if isinstance(body, dict) else '?'}")
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
