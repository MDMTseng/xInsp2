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
The pack plane is `key(string) -> entry`, so the canonical profile restricts map
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
  the pack layer's allocator.
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

## Reconciliation (2026-07-02, integration lead) — BINDING rulings

The two sections above were written independently (TS/Py vs C++). Where they
disagree, the following rulings are the profile; the alignment work is owned
by the ingress-canonicalizer task:

1. **NaN is NORMALIZED at encode and at canonicalize** to the single positive
   quiet-NaN pattern 0x7ff8000000000000 (the TS/Py choice). Cross-language
   byte-determinism wins over bit-pattern preservation; C++'s
   bitwise-identity behavior on NaN is to be aligned. ±Inf and -0.0 are
   preserved exactly (all sides already agree).
2. **Maps are STRING-KEYED, enforced**: encoders reject non-string keys by
   construction; the validating reader and canonicalize() REJECT foreign
   non-string keys (TS/Py MapKeyError semantics; C++ to add the check).
3. Integer boundaries: [−2^63, 2^63) → 0xd3; [2^63, 2^64) → 0xcf — all three
   already agree; int_ and uint_ of the same value are byte-identical.
4. Ext: canonical width ext32; mintable ONLY by the privileged C++ pack
   layer; TS/Py writers never emit ext; readers reject by default everywhere.
5. Duplicate map keys: currently unchecked in C++; canonicalize() SHOULD
   reject duplicates at ingress (hygiene) — assigned to the canonicalizer
   task with the rest.

## Alignment landed (2026-07-02, ingress-canonicalizer task 1c)

The five rulings above are now implemented in the C++ codec
(`backend/include/xi/xi_mp.hpp`). No spec incident surfaced: after alignment
all three implementations byte-agree on the shared vectors AND on the C++
golden fixtures. The three-way cross-check now runs on every build —
`canonical_xcheck` (C++ vs `contract/canonical-vectors.json`, 41 encode + 11
recanon), `canonical_xcheck_py` (Python decodes+recanonicalizes the C++
`protocol/fixtures/canonical/*.bin` goldens and the shared vectors), and the
Node leg in `ui-components/test/canonical-mp.mjs` (same goldens + vectors).

- **Ruling 1 (NaN)** — `Writer::float_` flattens ANY NaN (detected on the raw
  bits: exponent all-ones, mantissa non-zero, so signalling NaN is caught
  without FPU-quieting reliance) to `0x7ff8000000000000`; `canonicalize()`
  inherits it (it is the emit path). ±Inf and −0.0 are preserved. The
  `scalar_float` golden was UNCHANGED (its `std::nan("")` was already the
  canonical pattern), so no golden regen was needed for NaN.
- **Ruling 2 (string keys)** — `validate()` and `canonicalize()` reject a
  non-string map key (`Status::NonStringKey`, the twin of Python/TS
  `MapKeyError`). New golden `hostile_nonstring_key.bin`; both siblings reject
  it too.
- **Ruling 5 (duplicate keys) — the split.** `canonicalize()` (the ingress
  door) rejects duplicate keys (`Status::DuplicateKey`); `validate()` stays
  PERMISSIVE (structural well-formedness does not require uniqueness, and cheap
  dup-detection there is awkward). This is a C++-only ingress-hygiene check:
  the Python and TS codecs decode maps into native dict/object/Map, which
  DEDUPE silently rather than reject. That divergence is acceptable and
  deliberate — dup detection is a property of C++'s one-pass canonicalizer, not
  of the wire profile — so no duplicate-key fixture is added to the shared
  golden set (a Python "must reject" cross-check on such a fixture would fail by
  construction). Covered by the C++ unit test
  `canonicalize_rejects_duplicate_map_keys`.

The ingress edge itself (`backend/include/xi/xi_ingress.hpp`,
`xi::ingress::canonicalize_entry`) composes these into the doc-07 three-layer
boundary and is the ONLY public path from foreign bytes to a Pack entry; a
pool-handle ext (`kPoolHandleExtType`) is never imported, even under an
accept-list.

## Pack-shaped fail-loud (polaris2 wave-2, xi.pack@1 door)

When a plugin's pack-in/pack-out door (`xi.pack@1`, `xi_pack_proc_v1`) hits
a CONTRACT failure — a missing required entry, a wrong-typed one, a schema skew
— it does NOT return `XI_PACK_NULL`. `XI_PACK_NULL` is reserved for a HARD
internal failure (a caught crash / no pack plane). A contract failure is a
normal SEALED pack carrying a fail-loud error, so the caller ALWAYS gets a
pack to route to a verdict — the pack analogue of `xi::Record::na()` +
`xi::contract`'s `$fault`. The convention (SDK `xi::PackOut::fault` /
`xi::PackIn::is_fault`, keys in `xi::pack_contract`):

- `"$fault"`        — str, the reason code, REUSING the `xi_contract.hpp` codes
  (`missing_input` / `wrong_type` / `schema_mismatch`).
- `"$fault_key"`    — str, the offending entry key (optional).
- `"$fault_detail"` — str, a human message (optional).

A consumer checks `has("$fault")` before reading results. The reason codes are
identical to the Record path's `$fault.code`, so a script/health mapper treats a
Record NA and a Pack fault the same way. (Reserved `$`-prefixed keys stay out of
the plugin's declared schema keyset.)

### U1 addendum: provenance + the propagate contract (docs/new_gen/15)

The reserved-key set gained two provenance entries, and the fault convention
gained a PROPAGATION rule (constants + shared helpers:
`backend/include/xi/xi_pack_contract.hpp`, `xi::pack_contract` — the former
`xi_abi.hpp` constants moved there, spellings unchanged):

- `"$src"`  — str, the immediate producer: the instance whose seal minted the
  pack. Auto-stamped before seal by the door glue (`pack_door_abi`) on every
  NON-EMPTY door output (an untouched PackOut seals empty — the absence
  sentinel stays unstamped); explicit `PackOut::src()` /
  `ScriptPackBuilder::src()` for producer-chosen attribution.
  `Plugin::emit(PackOut&&)` stamps NOTHING — replay byte-identity
  (record_replay, doc 13 E3) and published emit shapes are producer
  contracts.
- `"$prov"` — str, the hop chain: instance names joined by `/`,
  oldest→newest (`cam0/det0/det1`). Door outputs append their hop to the
  input's chain (a lone `$src` seeds the chain).

PROPAGATE: `$fault` is the pack plane's ONE poison marker (there is
deliberately no pack `"$na"`). The host use-funnel (`use_pack_process_cb`)
short-circuits a fault input BEFORE the instance lookup — the plugin never
runs; the result is a NEW sealed pack (sealed packs are immutable) carrying
the original `$fault`/`$fault_key`/`$fault_detail` + `$seq`, with `$src` = the
hop and the hop appended to `$prov` — mirroring the Record path's
`Record::na(reason).set_src(name)` short-circuit. The push path stamps
nothing, ever (the byte-identical-dump guarantee stands). Script surface:
`ScriptPack::is_fault()/fault_reason()/fault_key()/fault_detail()/src()/prov()`,
`ScriptPackBuilder::fault()/src()`.

### Producer-domain type dispatch: bare `type`, not `$type` (polymorphic door)

One pack door MAY dispatch different behaviors on a kind entry in the request
pack — the same idiom the capability plane uses for `$v` (dispatch is
provider-internal; no funnel reads the discriminator). The blessed convention
for that discriminator is a BARE `type` str entry, NOT `$type`:

- The `$` prefix marks keys the FRAMEWORK owns — keys a host funnel or the
  door glue reads/stamps (`$fault` family, `$src`/`$prov`, `$seq`) or a
  plane-level contract defines (`$v`/`$probe`/`$versions` on the capability
  plane). A which-of-MY-behaviors switch is PRODUCER schema: each door names
  its own supported set, no host component interprets it, and it belongs in
  the plugin's declared keyset like any other input — which the rule above
  ("reserved `$`-prefixed keys stay out of the plugin's declared schema
  keyset") would forbid for a `$type`. Spelling it `$type` would claim
  framework semantics it doesn't have.
- Unknown `type` → the door answers a normal sealed fault pack:
  `$fault: "unsupported_type"`, `$fault_key: "type"`, plus an un-prefixed
  `types` str entry naming the supported set (comma-joined) — the
  producer-domain mirror of the capability plane's unsupported-`$v` reply
  (`$fault: "unsupported_version"` + `$versions`). A missing `type` on a door
  that requires one is the ordinary `missing_input`.

Reference example: `examples/qa_pack_poly_door` (the bundled `poly_door`
plugin: `type="measure"` → image stats, `type="annotate"` → derived overlay
entry, anything else → `unsupported_type` + `types`).
