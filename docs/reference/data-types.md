# Data types at the boundary — Pack, Image, typed I/O

What crosses between a script and a plugin: a **Pack** (a sealed, keyed, typed
container) carrying **Images** (refcounted pixel handles).
The mechanics live in [`../internals/pack-plane.md`](../internals/pack-plane.md)
(how a pack crosses zero-copy); this is the contract you author against.

## The Pack (`xi.pack@1`)

The universal container since THE CUT (ABI v12): a sealed, immutable set of
`key → typed entry` pairs. One *external* encoding everywhere — the same
canonical msgpack bytes on the WS wire (XEX1-v3) and on disk (`.xex1`). In
memory (pack-v3 slab) an inline entry stores that same canonical value, so
memory == wire and a serialization boundary is a copy, not a re-encode; a typed
read skips the fixed-width header at a known offset
([`../internals/pack-plane.md`](../internals/pack-plane.md)). Entry types:
`i64`, `f64`, `bool`, `str`, `bin`, and `mp` (one nested canonical-msgpack
subtree for arrays/maps — nesting is msgpack's job, not flattened keys), plus
the one non-scalar kind — a self-describing **`blob`** (`'XBD1'` head + a
canonical-msgpack descriptor + a 64B-aligned payload). An **`image`** is just a
convention `xi/image` blob (descriptor `{"t":"xi/image","w","h","c","dt"}`),
reached through the `image`-named convention helpers or the blob door. Blob door
access is the `xi.pack@4` interface; the earlier per-type `tensor`/`type_id`
surface (`xi.pack@3`) was **retired by the blob cut** (spec 30) and, like the
never-existed `@2`, answers NULL forever.

Each side of the boundary has its own facade over the same door:

- **Script side** (`xi_use.hpp` / `xi_script_pack.hpp`): `t.pack()` yields a
  `ScriptPack` — typed reads return `std::optional` (absence is explicit):

  ```cpp
  auto f = t.pack();                          // ScriptPack; empty if no pack
  int64_t seq = f.get_i64("seq").value_or(0);
  auto img = f.get_image("frame");            // zero-copy span + dims
  ```

  Build with `xi::ScriptPackBuilder` (`add_i64/f64/str/bool/bin`, `add_image`,
  `add_mp(key, xi::mp::Writer)`, then `seal()`), chain with
  `xi::use("det0").process(pack)`, push to a sink with
  `xi::use("expose").push(pack)`. See
  [`../guides/write-a-script.md`](../guides/write-a-script.md).
- **Plugin side** (`xi_abi.hpp`): the pack door
  `void process(xi::PackIn& in, xi::PackOut& out)` published by
  `XI_PLUGIN_PACK_DOOR`. `PackIn` reads (`i64(k)`, `i64_or(k, d)`, `image(k)`,
  `mp(k)`, …); `PackOut` builds fluently (`out.i64(...).image(...).mp(...)`).
  See [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md).

Semantics worth knowing:

- **Sealed = immutable.** A sealed pack never changes; routing/ordering
  identity are the pack's *own* entries, stamped by the producer before seal.
- **Fault is first-class** (the pack plane's one poison marker): a failure is a
  *normal sealed pack* carrying `$fault` — check `is_fault()` before reading
  results. Faults short-circuit the use-funnel host-side, so a plugin never
  sees poison. Full contract:
  [`../../docs/new_gen/15-pack-fault-semantics.md`](../new_gen/15-pack-fault-semantics.md).
- **Provenance rides along**: `$src` (immediate producer) and `$prov` (hop
  chain) are auto-stamped by the door glue on non-empty door outputs.
- **Declared keysets**: when the field set is fixed, read via a compile-checked
  schema — a plain struct with a constexpr `keys` array, read script-side via
  `ScriptTypedPack<Schema>` (`t.pack().typed<Schema>()`) — so key spelling
  can't drift. (The in-process `TypedPack<Schema>` / `PackSchema` container was
  deleted 2026-07-11 — `Pack`/`PackBuilder` is the only in-process container.)

### Reserved Pack keys

The framework reserves the `$`-prefixed key namespace. Constants + helpers live
in `xi::pack_contract` (`xi_pack_contract.hpp`; full contract:
`docs/new_gen/15-pack-fault-semantics.md`). `$fault` is the pack plane's ONE
poison marker — there is deliberately no pack `$na`.

| Key | Constant | Meaning |
|---|---|---|
| `$fault` | `pack_contract::kFault` | str reason code — the pack is POISONED; check `is_fault()` before reading results. `xi::contract` codes for contract failures, free-form producer codes otherwise. |
| `$fault_key` | `pack_contract::kFaultKey` | str, offending entry key (optional). |
| `$fault_detail` | `pack_contract::kFaultDetail` | str, human message (optional). |
| `$src` | `pack_contract::kSrc` | str, immediate producer — auto-stamped at seal by the door glue on NON-EMPTY door outputs (`emit()` never stamps; sources/scripts attribute explicitly via `src()`). |
| `$prov` | `pack_contract::kProv` | str, hop chain (`/`-joined, oldest→newest) — door hops append; the use-funnel appends on fault short-circuit too. |
| `$channel` | `pack_contract::kChannel` | str, staged-emit / `expose` sink lane selector (which channel a pack surfaces on). |
| `$seq` | `pack_contract::kSeq` | i64, ordering identity for sink deliveries — the pack's OWN entry, producer-stamped before seal (`b.add_i64("$seq", xi::run_id())`); never host-stamped, sealed packs are immutable. `propagate_fault` copies it forward. |
| `$stream` | `pack_contract::kStream` | i64, producer-chosen stream id — streaming-via-chunking convention (`docs/new_gen/18-stream-chunking-convention.md`): one logical payload as a sequence of sealed packs on one lane, all carrying the same id. |
| `$part` | `pack_contract::kPart` | i64, 0-based DENSE chunk index within a `$stream` (+1 per chunk; a gap is a protocol fault — no reorder tolerance on one lane). |
| `$eof` | `pack_contract::kEof` | bool, present-and-true on the LAST chunk only — the stream's only completion signal; missing `$eof` ends in a consumer-owned timeout fault. A mid-stream `$fault` pack poisons the whole stream. |

## `xi::Image`

An owning, refcounted 8-bit image buffer (`xi_image.hpp`).

| Member | Meaning |
|---|---|
| `width` / `height` / `channels` | `channels` = 1 (gray) / 3 (RGB) / 4 (RGBA) |
| `empty()` | true if any dim/channel is 0 — **the failure sentinel** |
| `read()` / `write()` / `size()` / `stride()` | packed bytes (`stride == width*channels`, rows contiguous); `read()` is const, `write()` mutable-when-uniquely-owned |
| `xi::as_cv_mat(img)` | **non-owning** `cv::Mat` view over the same bytes, no copy (`xi_cv.hpp`, free function) |

> **Channels are RGB, not BGR.** OpenCV assumes BGR — `cvtColor(..., COLOR_RGB2BGR)`
> before `imwrite`/`imshow` an `as_cv_mat()` view, or channels look swapped.

Construct: `Image(w,h,c)` (zero-filled) · `Image(w,h,c,data)` (copies) ·
`Image::create_in_pool(host,w,h,c)` (plugins: zero-copy pool-backed buffer to
*produce* a result) · `Image::adopt_pool_handle(host,h)` (zero-copy view over a
host handle). (The former SHM/worker branch was removed 2026-06; the `shm_*` ABI
fields stay null-wired for binary compat.)

**Decode:** file decoding is the `xi.image.decode` **capability** (the in-core
`xi::imread` and the `read_image_file` host slot were deleted at THE CUT). A
plugin that needs to decode PNG/JPEG/BMP bytes resolves the capability funnel
(`get_interface("xi.cap", 1)`) and calls `"xi.image.decode"` with a `bin "data"`
request — an `xi.image.decode` provider (the `imgcodec` plugin, as a project
instance or machine-wide via `--autoload-lib`) must be present. Scripts don't
decode: the frame rides in on the trigger's pack.

**OpenCV interop (both directions, `xi_cv.hpp`):**
- `xi::as_cv_mat(img)` — zero-copy borrow; the Mat must not outlive the Image.
- `xi::from_cv_mat(m)` — owning copy (8-bit 1/3/4-ch; non-continuous ROI is
  `clone()`d; empty/non-8-bit → empty Image). The supported way to return a
  computed `cv::Mat` lifetime-safely.

**`VAR`/`EMIT` were removed** — they **no longer compile** (compiler error C3861);
the per-run value/preview transport was removed from core. Surfacing values for
viewing goes through the shipped `expose` plugin. There is **no header and no
macro**: build a pack with `xi::ScriptPackBuilder`, tag it with the reserved key
`"$channel"` (a string channel id), and call `xi::use("expose").push(pack)`.
Display order = the pack's own key order (insertion order is preserved); stamp
`"$seq"` yourself (`b.add_i64("$seq", xi::run_id())`) for ordering.

## Typed I/O — names over the pack

The Record-era nominal-type vocabulary (`Number`, `Point`, `Pose`, `Roi`, …,
`xi_types.hpp`) and its generated `io.hpp` extractor/constructor facades were
**deleted at THE CUT** together with the Record plane. The typed surface of the pack
plane is the declared keyset: a plain schema struct with a constexpr `keys`
array fixes the contract's key slots at compile time,
`ScriptTypedPack<Schema>` (script side, `t.pack().typed<Schema>()`) reads those
slots with typed accessors, and a plugin's published key constants (e.g.
`blob_analysis_keys.h`) keep call sites literal-free. (The in-process
`TypedPack<Schema>`/`TypedPackBuilder`/`PackSchema` container was deleted
2026-07-11 with zero production consumers; `Pack`/`PackBuilder` is the one
in-process container, and `ScriptTypedPack` — key-based over the opaque
`xi_pack_v1` ABI — lives on unchanged.) See
[`../internals/pack-plane.md`](../internals/pack-plane.md) and
`examples/qa_pack_walk`.

## See also

- [`c-abi.md`](./c-abi.md) — the pack door at the raw ABI (`xi_pack_v1` + handles).
- `xi_pack.hpp` / `xi_pack_contract.hpp` / `xi_image.hpp` (+ opt-in `xi_cv.hpp`).
