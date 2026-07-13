# xInsp3 Data Plane — Uniform Keyed Binary Buffers, msgpack by Default

> [2026-07-14] **Storage model superseded by the v3 slab (packv3 branch):** scalars now store RAW in one slab and canonical msgpack is produced at the serialization edge (`canonical_value` walk) — the "memory ≈ wire" identity below is historical; see docs/internals/pack-plane.md.

> **Naming:** the container is **Pack** (`xi::Pack`, the `xi.pack@1` door). It
> was called **Frame** in the wave-2 pilots; the type/door/SDK surface was
> renamed Frame → Pack (zero image connotation) — the `"frame"` *image key* and
> the preview-frame wire wording are unrelated and unchanged.

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Adopted direction for the v3 data plane (maintainer decision). Supersedes the two-plane amendment in [`polaris2/00-synthesis.md`](./polaris2/00-synthesis.md) §2 by unifying it. Not a v2 retrofit |
| **Decision** | One pack container: `key → (type tag, binary buffer)`. No image/meta split — an image is an entry whose type happens to be an image format. **Everything structured encodes as msgpack — including image descriptors — under a canonical max-width profile** (all numbers fixed-width int64/float64; wide container markers). Pixels live raw in pool buffers referenced by a handle ext type. Zero-copy sharing (pool handles + refcounts) is retired to large buffers only; the small plane is immutable arena data freed with the pack |

## The proposal

The polaris2 visions converged on killing the shared-mutable JSON Record but
all four kept a two-plane design (pooled images + a typed metadata value).
This decision goes one step further: **there is only one kind of thing in a
pack** —

```
Pack = { key: string  →  entry }
entry = { type: string,  bytes: const span }
```

- `"frame"     → { type: "image/bgr8",  bytes: <msgpack: {w,h,c,dtype,stride, px:<pool-handle ext>}> }`
- `"threshold" → { type: "mp",          bytes: <msgpack: 128 (encoded 0xd3, 9 bytes)> }`
- `"blobs"     → { type: "mp",          bytes: <msgpack: [{area:…, cx:…}, …]> }`
- `"depth"     → { type: "tensor/f32",  bytes: <msgpack: {dtype,shape,stride, px:<pool-handle ext>}> }`

The core owns **buffers and dispatch, not images**: "image-ness" is a plugin's
interpretation of a tag + a self-contained header. This is the minimal-core
principle taken to its limit — the core's last piece of domain knowledge
(width/height/channels living in the pool) moves into the buffer itself.

## What it buys

1. **One ownership discipline.** Today's two refcount registries (ImagePool +
   DocRegistry) exist because images and metadata are different citizens.
   Unified, there is ONE pool (large buffers), ONE arena (small entries), one
   walk, one crash story: a caught fault drops the pack — arena freed in one
   shot, handles released by the pack's single owner. The
   leak-per-crash class (vision B/D's `xi_use.hpp:558` finding) cannot be
   expressed.
2. **Self-description survives** (the 02 constraint that made schemaless
   right): generic plugins (`record_save`, `expose`, `data_output`) walk
   entries and recurse into msgpack without knowing producers. Unknown type
   tags are opaque pass-through — forward compatibility by construction.
3. **Typed and fail-loud** (the 02 pain, solved): consumers check the tag and
   decode msgpack through generated/hand-written accessors (the existing
   `_keys.h`/`_io.h` discipline carries over unchanged as the view layer).
4. **New payload kinds for free**: tensors, depth, point clouds, ROI sets —
   a tag and a header convention, zero core changes. (Industry precedent:
   GenICam **GenDC** — keyed typed components in one container.)

## Why msgpack as the metadata default (the deciding properties)

- **The wire already speaks it.** XEX1 is hand-rolled msgpack; with msgpack
  as the in-memory encoding, the preview frame, the record/replay file, and
  the in-memory plane become **the same bytes** modulo whether large buffers
  are handles or inlined. Boundaries become copies, not transformations —
  `expose` stops re-encoding, replay stops re-parsing.
- **Self-describing + typed + compact**, native int64/bin/nested maps — the
  JSON warts the schema spike hit (int64-as-string, binary-as-category-error)
  don't exist on the pack plane. JSON remains at the human edges only:
  project/config files and the WS text envelope.
- **Implementations everywhere** (C/C++/TS/Py), and this repo already
  maintains golden fixtures + a width-discipline table for its msgpack subset
  (protocol/fixtures/binary, after the fixmap→map16 lesson) — the encoding is
  already governed here, and the canonical max-width profile (below) shrinks
  that discipline to a single width per type.
- **Write-once fits it.** msgpack's weakness is mutation/random access; the
  pack plane is write-once-read-many per pack (seal-then-share), so the
  weakness never engages. Readers scan KB-scale data; accessors cache offsets.

## The canonical encoding profile (max-width by default)

All numeric data is encoded at **maximum width, always**: integers as
`0xd3` int64 (or `0xcf` uint64 beyond int64 range), floats as `0xcb`
float64 — never the compact fixint/8/16/32 forms. Extended by the same
principle (recommended, one notch beyond the decision): container and
byte-string markers also use their widest forms (`map32`/`array32`/`str32`/
`bin32`). The output is 100% standard msgpack — any stock decoder reads it —
our encoders simply never compact. What this buys:

1. **Offset determinism → O(1) reads.** Write-once + a canonical field order
   (from the contract declaration) + fixed-width numbers means generated
   accessors can index `key → byte offset` once at seal time and then do
   direct offset reads — struct-grade random access without leaving msgpack.
2. **Size stability.** A value crossing 127/255/65535 no longer changes its
   encoded length: pack sizes are stable, in-place pre-seal patches are
   possible, replay files diff cleanly.
3. **One decoder path.** The width-boundary bug family (the fixmap→map16
   incident, review 10) loses most of its surface: encoders emit exactly one
   width per type, and the golden fixtures pin it.

Cost: a few hundred bytes per pack on a kilobyte-scale plane riding next to
megapixel buffers — noise. The profile applies to the pack plane everywhere
(memory, wire, disk — sameness is the point); the compact forms remain legal
to READ (stock msgpack), so foreign producers interop.

## Shape of the design (the three points that make it work)

**D1 — storage duality, API unity.** An entry's storage is either
arena-inline (small, the msgpack plane) or a pool handle (large, above a
threshold); both surface as a `const span`. Refcounts exist only for the
pool; the arena dies with the pack. In msgpack terms a handle is an **ext
type** ("pool ref") the walker resolves — so even the mixed pack serializes
naturally (inline the bytes on export, re-pool on import).

**D2 — dimensioned types are msgpack descriptors over raw pool buffers.**
An `image/*` or `tensor/*` entry is a small msgpack map ({w, h, c, dtype,
stride, px}) living in the arena; `px` is the pool-handle ext type, and the
PIXELS are their own raw pool buffer — aligned by the pool allocator, never
embedded behind a variable-length header. So `as_cv_mat` stays a zero-copy
wrap of the pool buffer; generic tools read shape/dtype without knowing what
an image is (one encoding to govern — no second npy-style format); and
alignment is a non-issue by construction. On export the walker resolves the
handle and inlines the pixels as `bin`; on import it re-pools them.

**D3 — nesting is msgpack's job.** Nested structures are just msgpack maps/
arrays inside an entry. No flattened key conventions, no second container
format; generic tools recurse.

## Ingress: foreign msgpack must prove itself at the domain edge

The canonical profile's payoffs (O(1) offset reads, one decoder path, sealed
immutability) are invariants the domain INTERIOR relies on without
re-checking. Therefore the edge must be total: **nothing enters the pack
plane unproven.** "Trust inside, prove at the boundary."

- **Canonicalize on ingress.** Foreign msgpack (a comms/MES plugin's inbound
  payloads, third-party camera chunk data, `json_source`-style feeds, old
  replay files) is decoded, validated, and re-encoded into the canonical
  profile in ONE pass by the producer plugin, via the SDK's canonicalizing
  entry constructor. Three layers: (a) structural well-formedness — bounded
  nesting depth, declared-vs-actual lengths, no trailing bytes (the review-09
  robustness class); (b) profile normalization — compact widths widened,
  canonical field order applied where the schema is known; (c) semantic
  validation against the contract schema when the entry claims a known type
  tag. Cost: one KB-scale decode+encode at the edge, paid once, buying the
  interior's right to never check again.
- **Constructive enforcement.** The canonicalizing constructor is the ONLY
  public way to build an entry from external bytes — there is no
  "insert raw span" path for untrusted data. The safe path is the only path,
  consistent with the rest of the architecture.
- **Ext types are rejected at the edge.** Pool handles are msgpack ext values;
  a forged handle in foreign data would be a fabricated pointer into the pool
  (use-after-free / type confusion by construction). The canonicalizer
  refuses (or downgrades to `bin`) any ext from an untrusted source — handles
  are mintable only by the domain's own allocator.
- **Replay files are untrusted by default.** Our own record files carry a
  canonical-profile version stamp, but disk corrupts and files get swapped:
  the default load path still canonicalizes. A verify-once-then-mmap fast
  path is a measured-performance option, not the default.

## Pack lifecycle (the concurrency story in one line each)

1. Producer builds entries into the pack's arena / grabs pool buffers.
2. **Seal** — the pack becomes immutable; only then does it cross the ABI.
3. Consumers (script, plugins, sinks) hold borrowed const views; outputs go
   into a NEW pack/arena (their own), never mutate the input.
4. Pack end: arena freed in one shot; pool handles released by the one
   owner. A caught plugin crash = drop the output pack; the input stays
   valid for the pipeline's error path. No reconciliation, no reserved-ref
   dance, no COW.

## Costs, honestly

- Scalar reads become tag-check + msgpack scan instead of a yyjson field
  read — same order of magnitude on KB data; accessor caching makes the
  common case an offset read. Writes get cheaper (arena bump-alloc beats
  mutable-DOM node allocation).
- Type-tag governance: a naming convention + registry in `contract/`
  (unknown = opaque). The contract tooling already exists.
- yyjson leaves the pack path entirely (stays for config/JSON edges); the
  ad-hoc `p["key"].as_int` style is replaced by the accessor layer — which is
  the direction 02 already adopted.
- This is the **v3 target representation**. v2 keeps the Record; the contract
  layer (keys/builders/extractors/schema stamp) is IDENTICAL in both worlds,
  which is what makes the eventual port mechanical.

## Relation to prior decisions

- **02 (plugin data contract)**: unchanged at the contract layer. What moves
  is the runtime representation underneath the same accessors.
- **polaris2 synthesis §2**: this refines the "immutable tagged arena value"
  amendment by erasing the remaining image/meta plane split.
- **Vision B/C's "kill the monolith" ABI amendment**: orthogonal, compatible —
  a `get_interface`-only ABI would expose exactly one pack interface:
  entries in, entries out.
