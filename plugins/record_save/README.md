# record_save

Saves inspection results to disk. **Bilingual** (docs/new_gen/10 gate P3): it
consumes both currencies and writes a different on-disk shape for each.

## Two doors

| Door | Entry point | Writes |
| --- | --- | --- |
| **Record** (legacy) | `process(const Record&)` | `<base>.json` + one `<base>_<key>.bmp` per image |
| **Pack** (canonical) | `process(PackIn&, PackOut&)` | one `<base>.xex1` per capture — the canonical XEX1-v2 dump |

Both honour the same config (`output_dir`, `naming_rule`, `enabled`); `<base>` is
`naming_rule` rendered with `{count}` / `{timestamp}`. The Record door is
**unchanged** — a Record-era project keeps getting its `.json` + `.bmp` exactly as
before. A project that routes packs here gets the `.xex1` dump instead.

record_save is **open-schema** (no `_keys`): it persists whatever entries a pack
carries, without producer knowledge, via the generic `count()/key_at()/tag_at()`
walk (doc 07 §2 self-description).

## The `.xex1` v2 format

The `.xex1` file is **the canonical XEX1-v2 frame, verbatim** — magic `XEX1`
followed by a canonical max-width msgpack map:

```
{ "v":2, "channel":<str>, "seq":<int>, "frame":{ <key>:<value>, ... } }
```

The reserved `$channel` / `$seq` entries are lifted to the top-level
`channel` / `seq` fields; every other entry lands in `frame` — scalars/str/bin as
their canonical value, nested msgpack verbatim, and an image as a descriptor map
`{ "w":.., "h":.., "c":.., "px":<bin pixels> }` (doc 07 D2: pool pixels inlined as
`bin` on export).

**This is not a private record format.** The bytes are produced by the *same*
encoder the `expose` plugin pushes on the wire — `plugins/expose/src/xex1_encode.hpp`
(the byte codec) driven by `xex1_pack_dump.hpp` (the generic pack walk). record_save
and expose share ONE implementation, held byte-identical by the goldens in
`protocol/fixtures/binary/` (the `xex1_fixtures` ctest). So **disk ≈ wire ≈ memory**
(doc 07): the persisted file is the canonical dump, provable by copy, not
re-serialization (`xex1_v2_identity_test`).

## Reading a dump back (replay)

`plugins/expose/src/xex1_pack_load.hpp` reads a `.xex1` file back into a sealed
`xi::Pack`:

```cpp
xi::xex1::LoadResult r = xi::xex1::load_pack_v2_file(path);
if (r.ok) { /* r.pack, r.channel, r.seq */ }
```

**Disk is untrusted** (doc 07 "Ingress"). A dump file may be truncated, forged, or
a foreign artifact, so the loader does **not** feed the bytes straight to
`add_mp` (which trusts its input). The whole body first goes through
`xi::ingress::canonicalize_entry` — the one blessed edge: bounded nesting,
declared-vs-actual length checks, string-keyed + duplicate-key rejection, and a
refusal to import a forged pool-handle `ext` (a fabricated pointer into the pool).
Only the trusted, canonicalized bytes are spliced into the rebuilt pack.

Because v2 stores an image as a msgpack map, load-back recovers the entry **tag**
by shape: a 4-key map whose keys are exactly `{w,h,c,px}` (ints + a bin) rebuilds
as an image; anything else as opaque nested msgpack. A genuine nested map shaped
exactly like an image descriptor is the one ambiguous case.

### Follow-up (not in this change)

`xex1_pack_load.hpp` is the replay **seed** — a load utility plus its round-trip
test (`record_save_pack_test`). Wiring it into a live replay **source** (a
`pack_mode` for a json_source-style replay feed, or the `cache` plugin) is the
remaining half of P3 and is tracked as a follow-up. A migration note for existing
(pre-v2) replay files is part of that work.

## Tests

`plugins/record_save_pack_test.cpp` drives the real DLL's pack door: it persists a
multi-entry pack (scalars + nested msgpack + image), asserts the file bytes equal
the shared encoder's output, loads it back through the untrusted-disk ingress and
asserts entries + image pixels are identical, checks a truncated file is refused,
and confirms the Record door still writes its `.json` — with the image pool
balanced at the end.
