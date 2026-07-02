# Canonical msgpack profile — implementation decision log

Where `docs/new_gen/07-uniform-keyed-buffer-plane.md` is silent or ambiguous,
the codec implementations record the minimal reasonable choice here. Each
independent implementation (C++, Python, TypeScript) APPENDS its decisions; the
maintainer reconciles conflicts at merge. If two implementations disagree on a
point below, that is a spec bug to resolve, not a codec bug to paper over.

---

## From the TypeScript + Python implementations (branch `polaris2/codec-xlang`)

Both `tools/xinsp2_py/xinsp2/canonical.py` and
`ui-components/src/canonical-mp.mjs` follow these; the shared vectors in
`contract/canonical-vectors.json` pin them across both languages.

### D-NaN — one canonical NaN bit pattern
IEEE-754 float64 has ~2^53 NaN encodings; a byte-deterministic profile must pick
one. **Chosen: `0x7ff8000000000000`** (positive-sign quiet NaN, zero payload) —
the platform-canonical quiet NaN. The encoder normalizes ANY NaN (any sign, any
payload, signalling or quiet) to this pattern. `recanonicalize` does the same, so
a foreign producer's odd NaN is flattened on ingress.

### D-NegZero — `-0.0` is preserved (NOT flattened to `+0.0`)
`-0.0` and `+0.0` are distinct IEEE bit patterns and both are legitimate values a
producer may have chosen. The encoder preserves the sign bit of zero:
`-0.0 -> cb8000000000000000`, `+0.0 -> cb0000000000000000`. (Contrast with NaN,
where the many-patterns problem forces normalization; zero has exactly two
patterns and they are meaningful, so we keep them.)

### D-MapKeys — map keys MUST be strings
The frame plane is `key(string) -> entry`, so the canonical profile restricts map
keys to strings. Both encode and decode/recanonicalize REJECT a non-string key
(`MapKeyError`) rather than coercing it. This also sidesteps JS's object-key
stringification (which would silently coerce an int key to a string) and keeps
key handling identical across languages. Non-string-keyed msgpack from a foreign
source is refused at ingress — consistent with "prove at the boundary".

### D-Ext — the default writer mints no ext; readers reject ext by default
Pool handles are the only ext users and are minted solely by the domain's
allocator, never by this codec. Therefore:
* `encode_canonical` / `encodeCanonical` THROW on an `Ext` value (`EncodeError`).
* `decode` rejects ext (`ExtNotAllowedError`) unless the caller passes
  `allow_ext=(codes…)` / `allowExt: [codes…]`, in which case ext decodes to an
  `Ext(code, data)` wrapper for inspection.
* `recanonicalize` rejects ext by default (its `allow_ext` defaults to empty),
  and even when ext is allowed to decode it cannot be re-emitted — canonicalizing
  ext-bearing data throws. This matches the doc's "ext types are rejected at the
  edge / downgraded" rule; downgrade-to-bin is left to the privileged ingress
  constructor, not the default codec.
* The ext TYPE byte is read as a **signed int8** (per the msgpack spec).

### D-JsNumber — TypeScript number policy (JS has one numeric type)
* **decode** returns a `number` for any integer within `Number.SAFE_INTEGER`, and
  a `BigInt` for integers beyond it (so full int64/uint64 range survives exactly).
  Floats always return `number`.
* **encode** accepts either: a `bigint` is always an integer (int64/uint64); a
  `number` that is `Number.isInteger(n)` encodes as an integer, otherwise as
  float64.
* Because JS cannot distinguish `5` from `5.0`, an integer-valued number encodes
  as an **int64 by default**. To force an integer-valued float onto the float64
  wire (e.g. `5.0`, or `-0.0` whose `Number.isInteger` is true), wrap it:
  `encodeCanonical(f64(5))`. `f64()` / the `Float64` class is the explicit escape
  hatch. Python needs no equivalent — its `float` type already distinguishes.
* `recanonicalize` is implemented in JS as a **byte-level transcoder** (reads the
  msgpack marker, re-emits canonically) rather than `encode(decode(...))`,
  precisely so the int-vs-float distinction is preserved from the input markers
  and never routed through the ambiguous JS `number`. Python's `recanonicalize`
  is `encode_canonical(decode(...))`, which is lossless there.

### D-Int64Boundary — integer marker selection
`INT64_MIN (-2^63) <= v <= INT64_MAX (2^63-1)` -> `0xd3` int64 (two's complement).
`INT64_MAX < v <= UINT64_MAX (2^64-1)` -> `0xcf` uint64. Anything below
`INT64_MIN` or above `UINT64_MAX` has no canonical fixed-width encoding and raises
`IntRangeError`. (Python ints are unbounded so this is a genuine range check; JS
uses `BigInt` for the out-of-`number`-range cases.)

### D-Validation — reader hardening (bounds before allocation)
`decode`/`recanonicalize` enforce, before allocating anything:
* **Depth**: nesting bounded by `max_depth` (default 64) -> `DepthLimitError`.
* **Length vs remaining**: a str/bin length, and an array/map element count, is
  checked against the bytes that remain. An array claiming N elements needs
  N >= 1 bytes remaining (>=1 byte/element); a map claiming N pairs needs
  2N bytes (>=2 bytes/pair). This defeats the "array32 claims 4 billion
  elements" preallocation bomb -> `TruncatedError`.
* **No trailing bytes** after the top-level value -> `TrailingBytesError`.
* `0xc1` (never-used) is rejected -> `DecodeError`.
* Readers accept the FULL standard width ladder (fixint, uint/int 8/16/32/64,
  float32, str/bin/array/map 8/16/32, fixext/ext) — canonicalization requires
  reading compact producers. float32 widens to float64 losslessly.
