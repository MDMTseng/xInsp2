# Data types at the boundary — Record, Image, typed I/O

What crosses between a script and a plugin: a **Record** (schema-less JSON data +
a named-image bag) carrying **Images** (refcounted pixel handles), optionally
dressed in **nominal types** for ergonomic wiring. The mechanics live in
[`../internals/data-layer.md`](../internals/data-layer.md) (how Record crosses
zero-copy) and [`../internals/typed-io.md`](../internals/typed-io.md) (how the
typed facades work); this is the contract you author against.

## `xi::Record`

The universal container: a yyjson document (schema-less JSON) + a `name → Image`
map. Build fluently, read by path:

```cpp
auto r = xi::Record()
    .image("edges", edge_img)
    .set("count", 5).set("pass", true)
    .set("roi", xi::Record().set("x", 4).set("y", 8));

r.get_int("count");              // 5
r["roi.x"].as_int(0);            // 4   — dotted path
r["points[2].score"].as_double();// path + array index
r.has_image("edges");            // true
```

- **Path expressions** (`a.b[0].c`) walk nested objects/arrays; a missing path
  returns the caller's default, never throws.
- **NA** is first-class: `xi::Record::na("reason")` is an empty result carrying a
  reason (`{"$na":"reason"}`); `r.is_na()` / `r.na_reason()`. It short-circuits
  `process()` and flows down the pipeline — see typed-io.
- **Provenance** (`$src` / `$prov`, reserved keys) records where data came from.
- Copies are cheap (a refcount bump) until mutated (copy-on-write) — see data-layer.

### Reserved Record keys

The framework reserves the `$`-prefixed key namespace. Don't name a plain field
`na`/`src`/`prov`/`channel`/`seq` (they'd collide). Each has a named
`constexpr` in `xi::Record` — prefer the constant over the literal in
first-party code.

| Key | Constant | Meaning |
|---|---|---|
| `$na` | `Record::kNaKey` | Not-available marker: `{"$na":"reason"}` (see `na()` / `is_na()` / `na_reason()`). |
| `$src` | `Record::kSrcKey` | Producing source/operator id (`set_src()` / `src()`). |
| `$prov` | `Record::kProvKey` | Per-field provenance map: field → source id (`set_prov()` / `prov_of()`). |
| `$channel` | `Record::kChannelKey` | Staged-emit / `expose` sink lane selector (which channel a record surfaces on). |
| `$seq` | `Record::kSeqKey` | Host-stamped arrival/run id, used to order sink deliveries. |

### Reserved Pack keys (the xi.pack@1 plane)

The pack plane reserves the same `$` namespace; constants + helpers live in
`xi::pack_contract` (`xi_pack_contract.hpp`; full contract:
`docs/new_gen/15-pack-fault-semantics.md`). `$fault` is the pack plane's ONE
poison marker — there is deliberately no pack `$na`.

| Key | Constant | Meaning |
|---|---|---|
| `$fault` | `pack_contract::kFault` | str reason code — the pack is POISONED; check `is_fault()` before reading results. `xi::contract` codes for contract failures, free-form producer codes otherwise. |
| `$fault_key` | `pack_contract::kFaultKey` | str, offending entry key (optional). |
| `$fault_detail` | `pack_contract::kFaultDetail` | str, human message (optional). |
| `$src` | `pack_contract::kSrc` | str, immediate producer — auto-stamped at seal by the door glue on NON-EMPTY door outputs (`emit()` never stamps; sources/scripts attribute explicitly via `src()`). |
| `$prov` | `pack_contract::kProv` | str, hop chain (`/`-joined, oldest→newest) — door hops append; the use-funnel appends on fault short-circuit too. |
| `$channel` / `$seq` | — | Same routing/ordering roles as on Records; on packs they are the pack's OWN entries (never host-stamped — sealed packs are immutable). `propagate_fault` copies `$seq` forward. |

## `xi::Image`

An owning, refcounted 8-bit image buffer (`xi_image.hpp`).

| Member | Meaning |
|---|---|
| `width` / `height` / `channels` | `channels` = 1 (gray) / 3 (RGB) / 4 (RGBA) |
| `empty()` | true if any dim/channel is 0 — **the failure sentinel** |
| `data()` / `size()` / `stride()` | packed bytes (`stride == width*channels`, rows contiguous) |
| `as_cv_mat()` | **non-owning** `cv::Mat` view over the same bytes (no copy) |

> **Channels are RGB, not BGR.** OpenCV assumes BGR — `cvtColor(..., COLOR_RGB2BGR)`
> before `imwrite`/`imshow` an `as_cv_mat()` view, or channels look swapped.

Construct: `Image(w,h,c)` (zero-filled) · `Image(w,h,c,data)` (copies) ·
`Image::create_in_pool(host,w,h,c)` (plugins: zero-copy pool-backed buffer to
*produce* a result) · `Image::adopt_pool_handle(host,h)` (zero-copy view over a
host handle). (The former SHM/worker branch was removed 2026-06; the `shm_*` ABI
fields stay null-wired for binary compat.)

**Load:** `xi::imread(path)` is **empty-on-failure, never throws** — a
missing/garbage/0-byte/non-image path returns `.empty()`, so a bad frame path
degrades cleanly. Always check.

**OpenCV interop (both directions):**
- `img.as_cv_mat()` — zero-copy borrow; the Mat must not outlive the Image.
- `xi::from_cv_mat(m)` — owning copy (8-bit 1/3/4-ch; non-continuous ROI is
  `clone()`d; empty/non-8-bit → empty Image). The supported way to return a
  computed `cv::Mat` lifetime-safely.

**`VAR`/`EMIT` were removed** — they **no longer compile** (compiler error C3861);
the per-run value/preview transport was removed from core. Surfacing values for
viewing now goes through the shipped `expose` plugin. There is **no header and no
macro**: build a plain `xi::Record`, tag it with the reserved key `"$channel"` (a
string channel id), and call `xi::use("expose").process(rec)`. Display order = the
record's own key order (insertion order is preserved); the host also stamps
`"$seq"` for ordering.

## Nominal types — names over Record

For ergonomic wiring, a small vocabulary wraps Record with a *name* (no fields
enforced; payload stays schema-less): `Number`, `Point`, `Vec2/3/4`, `Line`,
`Arc`, `Pose`, `Roi`, `Mat2/3/4`, `Region` (`xi/xi_types.hpp`). `Region` is iconic
— its data is a binary **mask** on the image channel, not JSON. A wrapper can be
NA and carries schema-less accessors (`pose.angle()`).

Plugins ship a header-only `io.hpp` with **extractor + constructor facades** that
mirror the manifest; they are **total (never throw)** — a missing field is NA, not
an exception:

```cpp
auto e  = blob_centroid_detector_io::extract(rec);                  // one getter per output
auto in = line_fit_io::build().current(e.orientation(k)).build();   // one setter per input
```

The compiler stops you wiring a `Line` into a `Pose` input, but `process()` itself
stays untyped (`Record` only) — types live in the wiring layer. Authors define
their own nominal types in their own `io.hpp`. Mechanics:
[`../internals/typed-io.md`](../internals/typed-io.md).

## See also

- [`c-abi.md`](./c-abi.md) — Record/Image at the raw ABI (`xi_record` + handles).
- `xi_image.hpp` / `xi_record.hpp` / `xi_types.hpp` (+ opt-in `xi_types_cv.hpp`).
