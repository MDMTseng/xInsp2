# xInsp3 Data Plane — Uniform Keyed Binary Buffers, msgpack by Default

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Adopted direction for the v3 data plane (maintainer decision). Supersedes the two-plane amendment in [`polaris2/00-synthesis.md`](./polaris2/00-synthesis.md) §2 by unifying it. Not a v2 retrofit |
| **Decision** | One frame container: `key → (type tag, binary buffer)`. No image/meta split — an image is an entry whose type happens to be an image format. **Structured metadata encodes as msgpack by default.** Zero-copy sharing (pool handles + refcounts) is retired to large buffers only; the small plane is immutable arena data freed with the frame |

## The proposal

The polaris2 visions converged on killing the shared-mutable JSON Record but
all four kept a two-plane design (pooled images + a typed metadata value).
This decision goes one step further: **there is only one kind of thing in a
frame** —

```
Frame = { key: string  →  entry }
entry = { type: string,  bytes: const span }
```

- `"frame"     → { type: "image/bgr8",  bytes: <npy-style header + pixels> }`
- `"threshold" → { type: "mp",          bytes: <msgpack: 128> }`
- `"blobs"     → { type: "mp",          bytes: <msgpack: [{area:…, cx:…}, …]> }`
- `"depth"     → { type: "tensor/f32",  bytes: <npy-style header + data> }`

The core owns **buffers and dispatch, not images**: "image-ness" is a plugin's
interpretation of a tag + a self-contained header. This is the minimal-core
principle taken to its limit — the core's last piece of domain knowledge
(width/height/channels living in the pool) moves into the buffer itself.

## What it buys

1. **One ownership discipline.** Today's two refcount registries (ImagePool +
   DocRegistry) exist because images and metadata are different citizens.
   Unified, there is ONE pool (large buffers), ONE arena (small entries), one
   walk, one crash story: a caught fault drops the frame — arena freed in one
   shot, handles released by the frame's single owner. The
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
  don't exist on the frame plane. JSON remains at the human edges only:
  project/config files and the WS text envelope.
- **Implementations everywhere** (C/C++/TS/Py), and this repo already
  maintains golden fixtures + a width-discipline table for its msgpack subset
  (protocol/fixtures/binary, after the fixmap→map16 lesson) — the encoding is
  already governed here.
- **Write-once fits it.** msgpack's weakness is mutation/random access; the
  frame plane is write-once-read-many per frame (seal-then-share), so the
  weakness never engages. Readers scan KB-scale data; accessors cache offsets.

## Shape of the design (the three points that make it work)

**D1 — storage duality, API unity.** An entry's storage is either
arena-inline (small, the msgpack plane) or a pool handle (large, above a
threshold); both surface as a `const span`. Refcounts exist only for the
pool; the arena dies with the frame. In msgpack terms a handle is an **ext
type** ("pool ref") the walker resolves — so even the mixed frame serializes
naturally (inline the bytes on export, re-pool on import).

**D2 — dimensioned types carry their own header.** `image/*`, `tensor/*`
buffers start with a fixed npy-style header (magic, dtype, shape, stride),
then raw data. Self-contained, language-neutral, fixed-offset (zero parse),
and `as_cv_mat` stays a zero-copy wrap. The type tag names the family; the
header carries the numbers.

**D3 — nesting is msgpack's job.** Nested structures are just msgpack maps/
arrays inside an entry. No flattened key conventions, no second container
format; generic tools recurse.

## Frame lifecycle (the concurrency story in one line each)

1. Producer builds entries into the frame's arena / grabs pool buffers.
2. **Seal** — the frame becomes immutable; only then does it cross the ABI.
3. Consumers (script, plugins, sinks) hold borrowed const views; outputs go
   into a NEW frame/arena (their own), never mutate the input.
4. Frame end: arena freed in one shot; pool handles released by the one
   owner. A caught plugin crash = drop the output frame; the input stays
   valid for the pipeline's error path. No reconciliation, no reserved-ref
   dance, no COW.

## Costs, honestly

- Scalar reads become tag-check + msgpack scan instead of a yyjson field
  read — same order of magnitude on KB data; accessor caching makes the
  common case an offset read. Writes get cheaper (arena bump-alloc beats
  mutable-DOM node allocation).
- Type-tag governance: a naming convention + registry in `contract/`
  (unknown = opaque). The contract tooling already exists.
- yyjson leaves the frame path entirely (stays for config/JSON edges); the
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
  a `get_interface`-only ABI would expose exactly one frame interface:
  entries in, entries out.
