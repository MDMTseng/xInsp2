// canonical-mp.mjs — the canonical max-width msgpack profile (xInsp3 data plane).
//
// Dependency-free ESM (like ws-client.mjs) so every client — browser webUI,
// node tools, the HMI shell — can share one codec. Implements the profile
// decided in docs/new_gen/07-uniform-keyed-buffer-plane.md: every value is
// encoded at MAXIMUM width, always. Integers are int64 (0xd3), or uint64 (0xcf)
// once past the int64 range; floats are float64 (0xcb); str/bin/array/map take
// their widest markers (str32/bin32/array32/map32). Output is 100% standard
// msgpack — the encoder simply never compacts — which is what lets a sealed
// frame index key->byte-offset once and then do O(1) offset reads.
//
// Three entry points mirror the Python SDK (tools/xinsp2_py/xinsp2/canonical.py):
//   encodeCanonical(value) -> Uint8Array
//   decode(bytes, {maxDepth, allowExt}) -> value
//   recanonicalize(bytes, {maxDepth, allowExt}) -> Uint8Array
//
// NUMBER POLICY (JS has one numeric type; documented decision):
//   * decode returns a `number` for any integer within Number.SAFE_INTEGER,
//     and a `BigInt` for integers beyond it (so int64/uint64 survive exactly).
//     Floats always return `number`.
//   * encode accepts either: a `bigint` is always an integer (int64/uint64); a
//     `number` that is Number.isInteger encodes as an integer, otherwise as
//     float64. To force an integer-VALUED float onto the float64 wire (e.g.
//     5.0, or -0.0, whose Number.isInteger is true), wrap it: `f64(5)`.
//   Rationale: JS cannot distinguish 5 from 5.0, so the integer reading wins by
//   default and F64 is the explicit escape hatch. See
//   contract/canonical-profile-notes.md.

const INT64_MIN = -(2n ** 63n);
const INT64_MAX = 2n ** 63n - 1n;
const UINT64_MAX = 2n ** 64n - 1n;

// Default nesting ceiling for decode/recanonicalize — deep enough for any real
// frame descriptor, shallow enough to refuse a depth bomb before the JS stack
// blows.
const DEFAULT_MAX_DEPTH = 64;

// The one canonical NaN bit pattern (positive quiet NaN), so frame bytes are
// deterministic across producers regardless of which NaN the caller had.
const CANONICAL_NAN_HI = 0x7ff80000;
const CANONICAL_NAN_LO = 0x00000000;

// ---- public value wrappers --------------------------------------------------

/** Force float64 encoding for an integer-valued number (5, -0.0, ...). */
export class Float64 {
  constructor(value) { this.value = value; }
}
export function f64(value) { return new Float64(value); }

/** A decoded ext value (only produced when allowExt permits its code). The
 *  default writer never emits ext, so encoding one throws. */
export class Ext {
  constructor(code, data) { this.code = code; this.data = data; }
}

// ---- error hierarchy (mirrors the Python typed exceptions) ------------------

export class CanonicalError extends Error {
  constructor(message) { super(message); this.name = new.target.name; }
}
export class EncodeError extends CanonicalError {}
export class IntRangeError extends EncodeError {}
export class DecodeError extends CanonicalError {}
export class TruncatedError extends DecodeError {}
export class TrailingBytesError extends DecodeError {}
export class DepthLimitError extends DecodeError {}
export class MapKeyError extends DecodeError {}
export class ExtNotAllowedError extends DecodeError {}

// ---- encoder ----------------------------------------------------------------

/**
 * Encode a native value to canonical max-width msgpack bytes.
 * Deterministic: for a plain object, own-key insertion order IS the wire order;
 * for a Map, its iteration order. That ordering is the caller's contract.
 * @returns {Uint8Array}
 */
export function encodeCanonical(value) {
  const w = new Writer();
  encodeInto(value, w);
  return w.take();
}

class Writer {
  constructor() { this.buf = new Uint8Array(64); this.len = 0; }
  _ensure(n) {
    if (this.len + n <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + n) cap *= 2;
    const next = new Uint8Array(cap);
    next.set(this.buf.subarray(0, this.len));
    this.buf = next;
  }
  u8(v) { this._ensure(1); this.buf[this.len++] = v & 0xff; }
  u32(v) {
    this._ensure(4);
    this.buf[this.len++] = (v >>> 24) & 0xff;
    this.buf[this.len++] = (v >>> 16) & 0xff;
    this.buf[this.len++] = (v >>> 8) & 0xff;
    this.buf[this.len++] = v & 0xff;
  }
  bytes(u8arr) { this._ensure(u8arr.length); this.buf.set(u8arr, this.len); this.len += u8arr.length; }
  take() { return this.buf.slice(0, this.len); }
}

const UTF8_ENCODER = new TextEncoder();

function encodeInto(value, w) {
  if (value === null || value === undefined) { w.u8(0xc0); return; }

  const t = typeof value;
  if (t === "boolean") { w.u8(value ? 0xc3 : 0xc2); return; }
  if (t === "bigint") { encodeInt(value, w); return; }
  if (t === "number") {
    if (Number.isInteger(value)) { encodeInt(BigInt(value), w); return; }
    encodeFloat64(value, w); return;
  }
  if (t === "string") { encodeStr(value, w); return; }
  if (value instanceof Float64) { encodeFloat64(value.value, w); return; }
  if (value instanceof Uint8Array) { encodeBin(value, w); return; }
  if (value instanceof Ext) {
    throw new EncodeError(
      "the default canonical writer cannot emit ext types " +
      "(pool handles are minted only by the domain allocator)");
  }
  if (Array.isArray(value)) {
    w.u8(0xdd); w.u32(value.length);
    for (const item of value) encodeInto(item, w);
    return;
  }
  if (value instanceof Map) {
    w.u8(0xdf); w.u32(value.size);
    for (const [k, v] of value) {
      if (typeof k !== "string") throw new EncodeError(`canonical map keys must be string, got ${typeof k}`);
      encodeStr(k, w); encodeInto(v, w);
    }
    return;
  }
  if (t === "object") {
    const keys = Object.keys(value);
    w.u8(0xdf); w.u32(keys.length);
    for (const k of keys) { encodeStr(k, w); encodeInto(value[k], w); }
    return;
  }
  throw new EncodeError(`cannot canonically encode value of type ${t}`);
}

function encodeInt(v, w) {
  // v is a BigInt.
  if (v >= INT64_MIN && v <= INT64_MAX) {
    w.u8(0xd3);
    const u = v < 0n ? (1n << 64n) + v : v;      // two's complement for negatives
    writeU64(u, w);
  } else if (v > INT64_MAX && v <= UINT64_MAX) {
    w.u8(0xcf);
    writeU64(v, w);
  } else {
    throw new IntRangeError(`integer ${v} is outside the canonical range [${INT64_MIN}, ${UINT64_MAX}]`);
  }
}

function writeU64(u, w) {
  // u is a non-negative BigInt < 2^64. Emit big-endian.
  const hi = Number((u >> 32n) & 0xffffffffn);
  const lo = Number(u & 0xffffffffn);
  w.u32(hi); w.u32(lo);
}

function encodeFloat64(num, w) {
  w.u8(0xcb);
  if (Number.isNaN(num)) { w.u32(CANONICAL_NAN_HI); w.u32(CANONICAL_NAN_LO); return; }
  const dv = new DataView(new ArrayBuffer(8));
  dv.setFloat64(0, num, false);            // big-endian; preserves -0.0 sign bit
  w.bytes(new Uint8Array(dv.buffer));
}

function encodeStr(s, w) {
  const b = UTF8_ENCODER.encode(s);
  w.u8(0xdb); w.u32(b.length); w.bytes(b);
}

function encodeBin(u8arr, w) {
  w.u8(0xc6); w.u32(u8arr.length); w.bytes(u8arr);
}

// ---- decoder ----------------------------------------------------------------

/**
 * Decode standard msgpack (canonical or compact) to a native value, enforcing
 * the boundary invariants: bounded depth, declared-vs-remaining length checks
 * before allocation, and no trailing bytes. Ext values throw unless their code
 * is listed in allowExt (then they decode to an Ext). See the NUMBER POLICY at
 * the top for how integers become number|BigInt.
 */
export function decode(bytes, { maxDepth = DEFAULT_MAX_DEPTH, allowExt = [] } = {}) {
  const r = new Reader(bytes, maxDepth, new Set(allowExt));
  const value = r.readValue(0);
  if (r.off !== r.b.length) {
    throw new TrailingBytesError(`${r.b.length - r.off} trailing byte(s) after the top-level value`);
  }
  return value;
}

/**
 * Decode (with full validation) and re-encode under the canonical profile — the
 * "canonicalize on ingress" pass. Implemented as a byte-level transcoder that
 * preserves the msgpack marker category, so the int-vs-float distinction JS
 * would otherwise lose (5 vs 5.0) is kept exactly. Ext values cannot be
 * re-emitted, so canonicalizing ext-bearing data throws.
 * @returns {Uint8Array}
 */
export function recanonicalize(bytes, { maxDepth = DEFAULT_MAX_DEPTH, allowExt = [] } = {}) {
  const r = new Reader(bytes, maxDepth, new Set(allowExt));
  const w = new Writer();
  r.transcodeValue(0, w);
  if (r.off !== r.b.length) {
    throw new TrailingBytesError(`${r.b.length - r.off} trailing byte(s) after the top-level value`);
  }
  return w.take();
}

const UTF8_DECODER = new TextDecoder("utf-8", { fatal: false });

class Reader {
  constructor(b, maxDepth, allowExt) {
    this.b = b;
    this.dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    this.off = 0;
    this.maxDepth = maxDepth;
    this.allowExt = allowExt;
  }

  _remaining() { return this.b.length - this.off; }
  _need(n) {
    if (n < 0 || this.off + n > this.b.length) {
      throw new TruncatedError(`need ${n} byte(s) at offset ${this.off} but only ${this._remaining()} remain`);
    }
    const start = this.off;
    this.off += n;
    return start;
  }
  _u8() { return this.b[this._need(1)]; }
  _uint(n) {
    const i = this._need(n);
    let v = 0n;
    for (let k = 0; k < n; k++) v = (v << 8n) | BigInt(this.b[i + k]);
    return v; // BigInt
  }
  _int(n) {
    const u = this._uint(n);
    const bits = BigInt(n * 8);
    return u >= (1n << (bits - 1n)) ? u - (1n << bits) : u;
  }

  // Represent an integer BigInt under the number policy: number if safe, else BigInt.
  _num(big) {
    if (big >= BigInt(Number.MIN_SAFE_INTEGER) && big <= BigInt(Number.MAX_SAFE_INTEGER)) {
      return Number(big);
    }
    return big;
  }

  readValue(depth) {
    if (depth > this.maxDepth) throw new DepthLimitError(`nesting exceeded maxDepth=${this.maxDepth}`);
    const c = this._u8();

    if (c <= 0x7f) return c;                                   // positive fixint
    if (c >= 0xe0) return c - 0x100;                           // negative fixint
    if (c >= 0x80 && c <= 0x8f) return this._readMap(c & 0x0f, depth);
    if (c >= 0x90 && c <= 0x9f) return this._readArray(c & 0x0f, depth);
    if (c >= 0xa0 && c <= 0xbf) return this._readStr(c & 0x1f);

    switch (c) {
      case 0xc0: return null;
      case 0xc2: return false;
      case 0xc3: return true;
      case 0xc4: return this._readBin(Number(this._uint(1)));
      case 0xc5: return this._readBin(Number(this._uint(2)));
      case 0xc6: return this._readBin(Number(this._uint(4)));
      case 0xca: { const i = this._need(4); return this.dv.getFloat32(i, false); }
      case 0xcb: { const i = this._need(8); return this.dv.getFloat64(i, false); }
      case 0xcc: return this._num(this._uint(1));
      case 0xcd: return this._num(this._uint(2));
      case 0xce: return this._num(this._uint(4));
      case 0xcf: return this._num(this._uint(8));
      case 0xd0: return this._num(this._int(1));
      case 0xd1: return this._num(this._int(2));
      case 0xd2: return this._num(this._int(4));
      case 0xd3: return this._num(this._int(8));
      case 0xd9: return this._readStr(Number(this._uint(1)));
      case 0xda: return this._readStr(Number(this._uint(2)));
      case 0xdb: return this._readStr(Number(this._uint(4)));
      case 0xdc: return this._readArray(Number(this._uint(2)), depth);
      case 0xdd: return this._readArray(Number(this._uint(4)), depth);
      case 0xde: return this._readMap(Number(this._uint(2)), depth);
      case 0xdf: return this._readMap(Number(this._uint(4)), depth);
      case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
        return this._readExt(1 << (c - 0xd4));
      case 0xc7: return this._readExt(Number(this._uint(1)));
      case 0xc8: return this._readExt(Number(this._uint(2)));
      case 0xc9: return this._readExt(Number(this._uint(4)));
      case 0xc1: throw new DecodeError("0xc1 is not a valid msgpack byte");
      default: throw new DecodeError(`unsupported msgpack byte 0x${c.toString(16)}`);
    }
  }

  _readStr(n) {
    const i = this._need(n);
    return UTF8_DECODER.decode(this.b.subarray(i, i + n));
  }
  _readBin(n) {
    const i = this._need(n);
    return this.b.slice(i, i + n);
  }
  _readExt(n) {
    const code = Number(this._int(1));
    const i = this._need(n);
    if (!this.allowExt.has(code)) throw new ExtNotAllowedError(`ext type ${code} rejected (not in allowExt)`);
    return new Ext(code, this.b.slice(i, i + n));
  }
  _readArray(n, depth) {
    if (n > this._remaining()) {
      throw new TruncatedError(`array claims ${n} element(s) but only ${this._remaining()} byte(s) remain`);
    }
    const a = new Array(n);
    for (let k = 0; k < n; k++) a[k] = this.readValue(depth + 1);
    return a;
  }
  _readMap(n, depth) {
    if (n > Math.floor(this._remaining() / 2)) {
      throw new TruncatedError(`map claims ${n} pair(s) but only ${this._remaining()} byte(s) remain`);
    }
    const o = {};
    for (let k = 0; k < n; k++) {
      const key = this.readValue(depth + 1);
      if (typeof key !== "string") throw new MapKeyError(`canonical map keys must be string, got ${typeof key}`);
      o[key] = this.readValue(depth + 1);
    }
    return o;
  }

  // ---- transcode: read one value, re-emit canonically, preserving type ----
  transcodeValue(depth, w) {
    if (depth > this.maxDepth) throw new DepthLimitError(`nesting exceeded maxDepth=${this.maxDepth}`);
    const c = this._u8();

    if (c <= 0x7f) { emitInt(BigInt(c), w); return; }
    if (c >= 0xe0) { emitInt(BigInt(c - 0x100), w); return; }
    if (c >= 0x80 && c <= 0x8f) { this._transMap(c & 0x0f, depth, w); return; }
    if (c >= 0x90 && c <= 0x9f) { this._transArray(c & 0x0f, depth, w); return; }
    if (c >= 0xa0 && c <= 0xbf) { emitStrBytes(this._strBytes(c & 0x1f), w); return; }

    switch (c) {
      case 0xc0: w.u8(0xc0); return;
      case 0xc2: w.u8(0xc2); return;
      case 0xc3: w.u8(0xc3); return;
      case 0xc4: emitBin(this._readBin(Number(this._uint(1))), w); return;
      case 0xc5: emitBin(this._readBin(Number(this._uint(2))), w); return;
      case 0xc6: emitBin(this._readBin(Number(this._uint(4))), w); return;
      case 0xca: { const i = this._need(4); encodeFloat64(this.dv.getFloat32(i, false), w); return; }
      case 0xcb: { const i = this._need(8); encodeFloat64(this.dv.getFloat64(i, false), w); return; }
      case 0xcc: emitInt(this._uint(1), w); return;
      case 0xcd: emitInt(this._uint(2), w); return;
      case 0xce: emitInt(this._uint(4), w); return;
      case 0xcf: emitInt(this._uint(8), w); return;
      case 0xd0: emitInt(this._int(1), w); return;
      case 0xd1: emitInt(this._int(2), w); return;
      case 0xd2: emitInt(this._int(4), w); return;
      case 0xd3: emitInt(this._int(8), w); return;
      case 0xd9: emitStrBytes(this._strBytes(Number(this._uint(1))), w); return;
      case 0xda: emitStrBytes(this._strBytes(Number(this._uint(2))), w); return;
      case 0xdb: emitStrBytes(this._strBytes(Number(this._uint(4))), w); return;
      case 0xdc: this._transArray(Number(this._uint(2)), depth, w); return;
      case 0xdd: this._transArray(Number(this._uint(4)), depth, w); return;
      case 0xde: this._transMap(Number(this._uint(2)), depth, w); return;
      case 0xdf: this._transMap(Number(this._uint(4)), depth, w); return;
      case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
        this._readExt(1 << (c - 0xd4)); return;     // throws unless allowed; ext can't be re-emitted
      case 0xc7: this._readExt(Number(this._uint(1))); return;
      case 0xc8: this._readExt(Number(this._uint(2))); return;
      case 0xc9: this._readExt(Number(this._uint(4))); return;
      case 0xc1: throw new DecodeError("0xc1 is not a valid msgpack byte");
      default: throw new DecodeError(`unsupported msgpack byte 0x${c.toString(16)}`);
    }
    // ext markers that reach here without throwing: the default writer mints no
    // ext, so canonicalizing them is impossible.
    throw new EncodeError("the default canonical writer cannot emit ext types");
  }

  _strBytes(n) {
    const i = this._need(n);
    return this.b.slice(i, i + n);
  }
  _transArray(n, depth, w) {
    if (n > this._remaining()) {
      throw new TruncatedError(`array claims ${n} element(s) but only ${this._remaining()} byte(s) remain`);
    }
    w.u8(0xdd); w.u32(n);
    for (let k = 0; k < n; k++) this.transcodeValue(depth + 1, w);
  }
  _transMap(n, depth, w) {
    if (n > Math.floor(this._remaining() / 2)) {
      throw new TruncatedError(`map claims ${n} pair(s) but only ${this._remaining()} byte(s) remain`);
    }
    w.u8(0xdf); w.u32(n);
    for (let k = 0; k < n; k++) {
      // Key must be a string in the canonical profile; transcode it as one.
      const kc = this._u8();
      let keyBytes;
      if (kc >= 0xa0 && kc <= 0xbf) keyBytes = this._strBytes(kc & 0x1f);
      else if (kc === 0xd9) keyBytes = this._strBytes(Number(this._uint(1)));
      else if (kc === 0xda) keyBytes = this._strBytes(Number(this._uint(2)));
      else if (kc === 0xdb) keyBytes = this._strBytes(Number(this._uint(4)));
      else throw new MapKeyError(`canonical map keys must be string (marker 0x${kc.toString(16)})`);
      emitStrBytes(keyBytes, w);
      this.transcodeValue(depth + 1, w);
    }
  }
}

// Emit helpers shared by the transcoder (operate on already-read payloads).
function emitInt(big, w) {
  if (big >= INT64_MIN && big <= INT64_MAX) {
    w.u8(0xd3);
    const u = big < 0n ? (1n << 64n) + big : big;
    writeU64(u, w);
  } else if (big > INT64_MAX && big <= UINT64_MAX) {
    w.u8(0xcf); writeU64(big, w);
  } else {
    throw new IntRangeError(`integer ${big} is outside the canonical range [${INT64_MIN}, ${UINT64_MAX}]`);
  }
}
function emitStrBytes(b, w) { w.u8(0xdb); w.u32(b.length); w.bytes(b); }
function emitBin(b, w) { w.u8(0xc6); w.u32(b.length); w.bytes(b); }
