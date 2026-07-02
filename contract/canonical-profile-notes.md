# Canonical msgpack profile — decision record

This file is the **tie-breaker record** for the canonical msgpack encoding
profile defined narratively in
[`docs/new_gen/07-uniform-keyed-buffer-plane.md`](../docs/new_gen/07-uniform-keyed-buffer-plane.md)
("The canonical encoding profile" and "Ingress" sections).

The C++ codec (`backend/include/xi/xi_mp.hpp`) and the sibling TS/Python codecs
are implemented **independently from the same doc**; at merge their output is
byte-compared against the golden fixtures
(`protocol/fixtures/canonical/*.bin`). Where the doc was silent or ambiguous,
the minimal reasonable choice was made and recorded HERE so the independent
implementations converge on the same bytes. Decisions only — the rationale
lives in the doc and the header.

## Numeric encoding

- **Integers are always `int64` (`0xd3`, 9 bytes)** when the value is in
  `[INT64_MIN, INT64_MAX]`. This includes every non-negative value in
  `[0, 2^63)` — a value's canonical form does **not** depend on the producer's
  C++ signedness. `uint`/`int` of the same value produce byte-identical output.
- **`uint64` (`0xcf`) is used ONLY for values strictly greater than
  `INT64_MAX`** (`[2^63, 2^64)`), which int64 cannot represent. This is the one
  case a non-negative integer is not int64.
- **Floats are always `float64` (`0xcb`, 9 bytes).** A foreign `float32` is
  widened to `float64` (exact — float32 ⊂ float64).
- **No integer/float cross-coercion.** An integer value stays an integer
  (int64/uint64); a float value stays float64. `1` encodes as int64; `1.0`
  encodes as float64. They are distinct canonical values.
- **Non-finite floats (NaN, ±Inf) are encoded natively as `float64`** bit
  patterns. No sentinel strings (the JSON-edge `"NaN"`/`"Infinity"` workaround
  does not apply to the msgpack plane). NaN payload bits are preserved verbatim
  by the canonicalizer (it copies the 8 raw bytes), so canonicalization is a
  bitwise identity on any float64 — including a signalling NaN.

## Strings, blobs, containers

- **`str` is always `str32` (`0xdb`)**, **`bin` is always `bin32` (`0xc6`)**,
  **arrays are always `array32` (`0xdd`)**, **maps are always `map32`
  (`0xdf`)** — the widest marker, even for empty/short values. An empty string
  is 5 header bytes + 0 data; an empty map is 5 bytes.
- **`str` bytes are emitted verbatim.** The codec does NOT validate or normalize
  UTF-8 (no NFC, no surrogate checks). "str vs bin" is the producer's type
  choice; canonicalization preserves it.
- **Map key order is preserved, never sorted.** Canonical field order is the
  CALLER's contract duty (two producers agreeing on a schema must agree on field
  order). The writer emits pairs in append order; the canonicalizer preserves a
  foreign map's key order. Schema-aware field-order normalization, if ever
  wanted, is a higher (contract) layer — not this codec.
- **Duplicate map keys are neither detected nor rejected** by the codec. If a
  schema forbids them, that is a contract-layer check.

## Ext types

- **Canonical ext width is `ext32` (`0xc9`) always**, even for a 1-byte payload
  (no fixext1..16, no ext8/ext16 on output). The 1-byte ext type follows the
  4-byte length, then the payload, per the msgpack ext32 layout.
- **The default builder cannot emit ext at all.** Ext is a separate, privileged
  method (`Writer::ext_privileged`). Pool-handle ext values are minted only by
  the frame layer's allocator.
- **Ext is rejected by default on ingress.** The validating reader / canonicalizer
  reject any ext unless its type is on an explicit accept-list (privileged
  callers), or the policy is set to strip unknown ext down to `bin`.

## Reader bounds (structural validation)

- **Default max nesting depth is 64.** A container one level deeper than the
  bound is rejected (`depth-exceeded`) before its children are read. Depth counts
  container nesting; the top-level value is depth 0.
- **Declared length is checked against remaining bytes BEFORE the payload/child
  is taken** (`length-overflow` for str/bin/ext; a container with an impossibly
  large count fails as `truncated` once the buffer is exhausted, in work bounded
  by the buffer size, not the declared count).
- **Trailing bytes after the top-level value are rejected** (`trailing-bytes`).
  A valid buffer is exactly one canonical value with nothing after it.
- **The never-used byte `0xc1` is rejected** (`reserved-byte`).
- **str/bin/ext payloads are returned as zero-copy views** into the input
  buffer; the codec allocates nothing on the read path.

## Reader input width families (what it ACCEPTS, vs what it emits)

The reader accepts the FULL standard msgpack width families — every integer,
string, binary, array, map, and ext width, plus fixint/fixstr/fixarray/fixmap
and float32 — so it reads both our canonical output and foreign compact input.
The writer emits only the single canonical width per type listed above. Reading
compact is required for the canonicalizer (the future ingress core).
