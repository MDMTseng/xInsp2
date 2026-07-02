"""Canonical max-width msgpack profile (xInsp3 data plane).

Implements the encoding profile decided in
`docs/new_gen/07-uniform-keyed-buffer-plane.md`: every value on the frame plane
is encoded at MAXIMUM width, always. Integers are `0xd3` int64 (or `0xcf`
uint64 once they exceed the int64 range), floats are `0xcb` float64, and the
container / byte-string markers use their widest forms (`map32` / `array32` /
`str32` / `bin32`). The output is 100% standard msgpack — any stock decoder
reads it — the encoder simply never compacts. That determinism is what lets a
sealed frame index `key -> byte offset` once and then do O(1) offset reads.

Three public entry points:

* :func:`encode_canonical` — a native Python value -> canonical bytes. The
  default writer CANNOT emit ext types (pool handles are minted only by the
  domain's own allocator, never by this codec).
* :func:`decode` — canonical *or* compact standard msgpack -> a native value,
  with the boundary validation the domain interior relies on: bounded nesting
  depth, declared-vs-actual lengths checked BEFORE allocation, and no trailing
  bytes. Ext values are rejected unless explicitly allowed.
* :func:`recanonicalize` — foreign/compact msgpack bytes -> canonical bytes, in
  one validated pass (the SDK's "canonicalize on ingress" operation).

Errors are typed and derive from :class:`CanonicalError` (a ``ValueError``), in
the same fail-loud, catchable style as the WS client's `ProtocolError` family.
"""
from __future__ import annotations

import math
import struct
from typing import Any, Iterable, NamedTuple

__all__ = [
    "encode_canonical",
    "decode",
    "recanonicalize",
    "Ext",
    "CanonicalError",
    "EncodeError",
    "IntRangeError",
    "DecodeError",
    "TruncatedError",
    "TrailingBytesError",
    "DepthLimitError",
    "MapKeyError",
    "ExtNotAllowedError",
    "INT64_MIN",
    "INT64_MAX",
    "UINT64_MAX",
]

# Int64 / uint64 boundaries. Python ints are unbounded, so encoding must
# range-check every integer and pick the marker per the profile.
INT64_MIN = -(2 ** 63)
INT64_MAX = 2 ** 63 - 1
UINT64_MAX = 2 ** 64 - 1

# The one canonical NaN bit pattern. IEEE-754 float64 has many NaN encodings;
# emitting a stable one keeps frame bytes deterministic across producers
# (recorded decision — see contract/canonical-profile-notes.md).
_CANONICAL_NAN = struct.pack(">Q", 0x7FF8000000000000)

# Default nesting-depth ceiling for decode/recanonicalize. Deep enough for any
# real frame descriptor, shallow enough that a "depth bomb" is refused before it
# exhausts the stack.
DEFAULT_MAX_DEPTH = 64


class Ext(NamedTuple):
    """A decoded msgpack ext value (only produced when ``allow_ext`` permits it).

    The default codec never *emits* ext — pool handles are privileged and minted
    only by the domain's allocator — so passing an :class:`Ext` to
    :func:`encode_canonical` is an error.
    """
    code: int
    data: bytes


# ---- error hierarchy ---------------------------------------------------------

class CanonicalError(ValueError):
    """Base class for every canonical-codec failure (a ``ValueError``)."""


class EncodeError(CanonicalError):
    """A value cannot be encoded under the canonical profile (bad type, or an
    ext value the default writer is not allowed to emit)."""


class IntRangeError(EncodeError):
    """An integer falls outside ``[INT64_MIN, UINT64_MAX]`` and so has no
    canonical fixed-width encoding."""


class DecodeError(CanonicalError):
    """Base class for malformed-input failures during decode/recanonicalize."""


class TruncatedError(DecodeError):
    """The input ended mid-value, or a declared length ran past the buffer."""


class TrailingBytesError(DecodeError):
    """A complete value was decoded but bytes remained after it."""


class DepthLimitError(DecodeError):
    """Nesting exceeded ``max_depth`` (a depth bomb)."""


class MapKeyError(DecodeError):
    """A map key was not a string. The canonical frame plane is string-keyed."""


class ExtNotAllowedError(DecodeError):
    """An ext value was present but its type code was not in ``allow_ext``.
    Forged ext values are fabricated pool pointers, so they are refused at the
    boundary by default."""


# ---- encoder -----------------------------------------------------------------

def encode_canonical(obj: Any) -> bytes:
    """Encode ``obj`` to canonical max-width msgpack bytes.

    Deterministic: dict iteration order (insertion order) IS the wire order —
    that ordering is the caller's contract. Integers become int64 (``0xd3``) or,
    once past ``INT64_MAX``, uint64 (``0xcf``); floats become float64 (``0xcb``);
    str/bin/array/map always take their widest markers.

    Raises :class:`EncodeError` for unsupported types (including :class:`Ext` —
    the default writer never mints ext) and :class:`IntRangeError` for integers
    outside ``[INT64_MIN, UINT64_MAX]``.
    """
    out = bytearray()
    _encode_into(obj, out)
    return bytes(out)


def _encode_into(obj: Any, out: bytearray) -> None:
    # bool must precede int: bool is an int subclass in Python.
    if obj is None:
        out.append(0xC0)
    elif obj is True:
        out.append(0xC3)
    elif obj is False:
        out.append(0xC2)
    elif isinstance(obj, int):
        _encode_int(obj, out)
    elif isinstance(obj, float):
        out.append(0xCB)
        out += _CANONICAL_NAN if math.isnan(obj) else struct.pack(">d", obj)
    elif isinstance(obj, str):
        b = obj.encode("utf-8")
        out.append(0xDB)
        out += struct.pack(">I", len(b))
        out += b
    elif isinstance(obj, (bytes, bytearray, memoryview)):
        b = bytes(obj)
        out.append(0xC6)  # bin32 always
        out += struct.pack(">I", len(b))
        out += b
    elif isinstance(obj, Ext):
        # Checked before list/tuple: Ext is a NamedTuple (a tuple subclass).
        raise EncodeError(
            "the default canonical writer cannot emit ext types "
            "(pool handles are minted only by the domain allocator)"
        )
    elif isinstance(obj, (list, tuple)):
        out.append(0xDD)
        out += struct.pack(">I", len(obj))
        for item in obj:
            _encode_into(item, out)
    elif isinstance(obj, dict):
        out.append(0xDF)
        out += struct.pack(">I", len(obj))
        for key, value in obj.items():
            if not isinstance(key, str):
                raise EncodeError(
                    f"canonical map keys must be str, got {type(key).__name__}"
                )
            kb = key.encode("utf-8")
            out.append(0xDB)
            out += struct.pack(">I", len(kb))
            out += kb
            _encode_into(value, out)
    else:
        raise EncodeError(f"cannot canonically encode value of type {type(obj).__name__}")


def _encode_int(v: int, out: bytearray) -> None:
    if INT64_MIN <= v <= INT64_MAX:
        out.append(0xD3)
        out += struct.pack(">q", v)
    elif INT64_MAX < v <= UINT64_MAX:
        out.append(0xCF)
        out += struct.pack(">Q", v)
    else:
        raise IntRangeError(
            f"integer {v} is outside the canonical range "
            f"[{INT64_MIN}, {UINT64_MAX}]"
        )


# ---- decoder -----------------------------------------------------------------

def decode(data: bytes, *, max_depth: int = DEFAULT_MAX_DEPTH,
           allow_ext: Iterable[int] = ()) -> Any:
    """Decode standard msgpack ``data`` (canonical or compact) to a native value.

    Accepts the FULL standard msgpack width ladder — canonicalization needs to
    read compact producers — while enforcing the boundary invariants: nesting is
    bounded by ``max_depth``, every declared length is checked against the bytes
    that remain *before* anything is allocated, and no trailing bytes may follow
    the top-level value.

    Ints decode to ``int`` (uint64 above ``INT64_MAX`` stays a Python int),
    floats to ``float``, str to ``str``, bin to ``bytes``, arrays to ``list``,
    maps to ``dict`` (string keys only). Ext values raise
    :class:`ExtNotAllowedError` unless their code is in ``allow_ext``, in which
    case they decode to an :class:`Ext`.

    Raises the :class:`DecodeError` family on malformed input.
    """
    reader = _Reader(data, max_depth, frozenset(allow_ext))
    value = reader.read_value(0)
    if reader.off != len(data):
        raise TrailingBytesError(
            f"{len(data) - reader.off} trailing byte(s) after the top-level value"
        )
    return value


def recanonicalize(data: bytes, *, max_depth: int = DEFAULT_MAX_DEPTH,
                   allow_ext: Iterable[int] = ()) -> bytes:
    """Decode ``data`` (with full boundary validation) and re-encode it under the
    canonical profile — the "canonicalize on ingress" pass.

    Compact widths are widened, non-finite floats are normalized, and trailing
    bytes / depth bombs / truncation are rejected. Because Python preserves the
    int-vs-float distinction through :func:`decode`, this is exactly
    ``encode_canonical(decode(data))`` — but note that ext values, even when
    ``allow_ext`` lets them decode, cannot be re-emitted (the default writer
    mints no ext), so canonicalizing ext-bearing data raises :class:`EncodeError`.
    """
    return encode_canonical(decode(data, max_depth=max_depth, allow_ext=allow_ext))


class _Reader:
    __slots__ = ("b", "off", "max_depth", "allow_ext")

    def __init__(self, b: bytes, max_depth: int, allow_ext: frozenset):
        self.b = b
        self.off = 0
        self.max_depth = max_depth
        self.allow_ext = allow_ext

    def _remaining(self) -> int:
        return len(self.b) - self.off

    def _need(self, n: int) -> int:
        start = self.off
        end = start + n
        if end > len(self.b) or n < 0:
            raise TruncatedError(
                f"need {n} byte(s) at offset {start} but only {self._remaining()} remain"
            )
        self.off = end
        return start

    def _u8(self) -> int:
        i = self._need(1)
        return self.b[i]

    def read_value(self, depth: int) -> Any:
        if depth > self.max_depth:
            raise DepthLimitError(f"nesting exceeded max_depth={self.max_depth}")
        c = self._u8()

        if c <= 0x7F:
            return c                                    # positive fixint
        if c >= 0xE0:
            return c - 0x100                            # negative fixint
        if 0x80 <= c <= 0x8F:
            return self._read_map(c & 0x0F, depth)      # fixmap
        if 0x90 <= c <= 0x9F:
            return self._read_array(c & 0x0F, depth)    # fixarray
        if 0xA0 <= c <= 0xBF:
            return self._read_str(c & 0x1F)             # fixstr

        if c == 0xC0:
            return None
        if c == 0xC2:
            return False
        if c == 0xC3:
            return True
        if c == 0xC4:
            return self._read_bin(self._uint(1))
        if c == 0xC5:
            return self._read_bin(self._uint(2))
        if c == 0xC6:
            return self._read_bin(self._uint(4))
        if c == 0xCA:
            i = self._need(4)
            return struct.unpack_from(">f", self.b, i)[0]
        if c == 0xCB:
            i = self._need(8)
            return struct.unpack_from(">d", self.b, i)[0]
        if c == 0xCC:
            return self._uint(1)
        if c == 0xCD:
            return self._uint(2)
        if c == 0xCE:
            return self._uint(4)
        if c == 0xCF:
            return self._uint(8)
        if c == 0xD0:
            return self._int(1)
        if c == 0xD1:
            return self._int(2)
        if c == 0xD2:
            return self._int(4)
        if c == 0xD3:
            return self._int(8)
        if c == 0xD9:
            return self._read_str(self._uint(1))
        if c == 0xDA:
            return self._read_str(self._uint(2))
        if c == 0xDB:
            return self._read_str(self._uint(4))
        if c == 0xDC:
            return self._read_array(self._uint(2), depth)
        if c == 0xDD:
            return self._read_array(self._uint(4), depth)
        if c == 0xDE:
            return self._read_map(self._uint(2), depth)
        if c == 0xDF:
            return self._read_map(self._uint(4), depth)
        if c in (0xD4, 0xD5, 0xD6, 0xD7, 0xD8):         # fixext 1/2/4/8/16
            return self._read_ext(1 << (c - 0xD4))
        if c == 0xC7:
            return self._read_ext(self._uint(1))
        if c == 0xC8:
            return self._read_ext(self._uint(2))
        if c == 0xC9:
            return self._read_ext(self._uint(4))
        if c == 0xC1:
            raise DecodeError("0xc1 is not a valid msgpack byte")
        raise DecodeError(f"unsupported msgpack byte 0x{c:02x}")

    def _uint(self, n: int) -> int:
        i = self._need(n)
        return int.from_bytes(self.b[i:i + n], "big", signed=False)

    def _int(self, n: int) -> int:
        i = self._need(n)
        return int.from_bytes(self.b[i:i + n], "big", signed=True)

    def _read_str(self, n: int) -> str:
        i = self._need(n)
        return self.b[i:i + n].decode("utf-8")

    def _read_bin(self, n: int) -> bytes:
        i = self._need(n)
        return bytes(self.b[i:i + n])

    def _read_ext(self, n: int) -> Ext:
        code = self._int(1)  # ext type is a signed int8 per the spec
        i = self._need(n)
        if code not in self.allow_ext:
            raise ExtNotAllowedError(f"ext type {code} rejected (not in allow_ext)")
        return Ext(code, bytes(self.b[i:i + n]))

    def _read_array(self, n: int, depth: int) -> list:
        # Each element is >= 1 byte, so a claimed count above the bytes that
        # remain is a lie: refuse it before allocating an n-slot list.
        if n > self._remaining():
            raise TruncatedError(
                f"array claims {n} element(s) but only {self._remaining()} byte(s) remain"
            )
        return [self.read_value(depth + 1) for _ in range(n)]

    def _read_map(self, n: int, depth: int) -> dict:
        # Each pair is >= 2 bytes (>=1-byte key + >=1-byte value).
        if n > self._remaining() // 2:
            raise TruncatedError(
                f"map claims {n} pair(s) but only {self._remaining()} byte(s) remain"
            )
        out: dict = {}
        for _ in range(n):
            key = self.read_value(depth + 1)
            if not isinstance(key, str):
                raise MapKeyError(
                    f"canonical map keys must be str, got {type(key).__name__}"
                )
            out[key] = self.read_value(depth + 1)
        return out
