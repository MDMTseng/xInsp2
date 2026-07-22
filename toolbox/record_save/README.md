# record_save

Saves inspection results to disk. **Bilingual** (docs/new_gen/10 gate P3): it
consumes both currencies and writes a different on-disk shape for each.

## Two doors

| Door | Entry point | Writes |
| --- | --- | --- |
| **Record** (legacy) | `process(const Record&)` | `<base>.json` + one `<base>_<key>.bmp` per image |
| **Pack** (canonical) | `process(PackIn&, PackOut&)` | one `<base>.xex1` per capture — the canonical XEX1-v3 dump |

Both honour the same config (`output_dir`, `naming_rule`, `enabled`); `<base>` is
`naming_rule` rendered with `{count}` / `{timestamp}`. The Record door is
**unchanged** — a Record-era project keeps getting its `.json` + `.bmp` exactly as
before. A project that routes packs here gets the `.xex1` dump instead.

record_save is **open-schema** (no `_keys`): it persists whatever entries a pack
carries, without producer knowledge, via the generic `count()/key_at()/tag_at()`
walk (doc 07 §2 self-description).

## The `.xex1` v3 format

The `.xex1` file is **the canonical XEX1-v3 frame, verbatim** — magic `XEX1`
followed by a canonical max-width msgpack map:

```
{ "v":3, "channel":<str>, "seq":<int>, "frame":{ <key>:[<tag>, <value>], ... } }
```

The reserved `$channel` / `$seq` entries are lifted to the top-level
`channel` / `seq` fields; every other entry lands in `frame` as a
**`[tag, value]` pair** — `tag` is the entry's pack-plane `XI_PACK_TAG_*`
(i64 0 / f64 1 / str 2 / bin 3 / image 4 / mp 5), `value` is the canonical
bytes: scalars/str/bin as their canonical value, nested msgpack verbatim, and an
image as a descriptor map `{ "w":.., "h":.., "c":.., "px":<bin pixels> }`
(doc 07 D2: pool pixels inlined as `bin` on export).

> **Why the tag (v3 vs the v2 draft).** The tagless v2 draft dumped bare values,
> so on readback an image descriptor was indistinguishable from a genuine nested
> map of the same shape — the loader guessed by shape (the P1 bonus finding).
> v3 puts the tag on the wire (+14 bytes/entry in the max-width profile), so the
> entry type is recovered exactly. The version bump makes stale readers fail
> closed; the v2 draft never shipped beyond the polaris2 line and is refused by
> the loader (see docs/new_gen/13-replay-file-migration.md).

**This is not a private record format.** The bytes are produced by the *same*
encoder the `expose` plugin pushes on the wire — `toolbox/expose/src/xex1_encode.hpp`
(the byte codec) driven by `xex1_pack_dump.hpp` (the generic pack walk). record_save
and expose share ONE implementation, held byte-identical by the goldens in
`protocol/fixtures/binary/` (the `xex1_fixtures` ctest). So **disk ≈ wire ≈ memory**
(doc 07): the persisted file is the canonical dump, provable by copy, not
re-serialization (`xex1_v2_identity_test`).

## Reading a dump back (replay)

The parse/build split (one parse, two build sites):

* `toolbox/expose/src/xex1_pack_parse.hpp` — the **plugin-safe parse**: magic +
  version gate, the untrusted-disk ingress edge, the tagged entry walk. Used by
  the `record_replay` source plugin (which rebuilds through the host `xi.pack@1`
  builder and emits into the graph).
* `toolbox/expose/src/xex1_pack_load.hpp` — the **host-side build**: parse +
  `xi::PackBuilder` into a sealed `xi::Pack` (tests, host tooling):

```cpp
xi::xex1::LoadResult r = xi::xex1::load_pack_v3_file(path);
if (r.ok) { /* r.pack, r.channel, r.seq */ }
```

**Disk is untrusted** (doc 07 "Ingress"). A dump file may be truncated, forged, or
a foreign artifact, so the parser does **not** feed the bytes straight to
`add_mp` (which trusts its input). The whole body first goes through
`xi::ingress::canonicalize_entry` — the one blessed edge: bounded nesting,
declared-vs-actual length checks, string-keyed + duplicate-key rejection, and a
refusal to import a forged pool-handle `ext` (a fabricated pointer into the pool).
On top of that, the v3 parser **enforces tag/value agreement** (a tag that
contradicts its value's msgpack kind refuses the whole file) and **image dim
sanity** (`w*h*c` must equal the pixel payload length — a forged descriptor
cannot make the pack builder read out of bounds). Only trusted, canonicalized
bytes are spliced into the rebuilt pack.

The entry **tag rides the wire** in v3, so load-back recovers every entry's type
exactly — the v2 draft's guess-by-shape (and its one ambiguous case) is gone.

The replay **source** is `toolbox/record_replay` — it closes the
record → save → replay loop (`record_replay_pack_test` proves the replayed packs
byte-identical to the originals). Migration for pre-v3 files:
docs/new_gen/13-replay-file-migration.md.

## Tests

`toolbox/record_save/tests/record_save_pack_test.cpp` drives the real DLL's pack door: it persists a
multi-entry pack (scalars + nested msgpack + image), asserts the file bytes equal
the shared encoder's output, loads it back through the untrusted-disk ingress and
asserts entries + image pixels are identical, checks a truncated file is refused,
and confirms the Record door still writes its `.json` — with the image pool
balanced at the end.
