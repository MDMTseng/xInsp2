# Data layer — the sealed keyed-buffer Pack plane

**Shipped design-of-record (ABI v12; storage = the pack-v3 slab, packv3
branch).** How plugin/script data crosses every (in-process) plugin boundary
as a **Pack** — one sealed, keyed, typed container whose **canonical-msgpack
wire form (XEX1-v3) and disk form (`.xex1`) are the same bytes**,
stored inline as those canonical bytes (memory == wire) and spliced verbatim at
the serialization edge — and how the host refcounts it across the ABI.

> **THE CUT (v12).** The data layer this page describes REPLACED the Record/doc
> plane. THE CUT deleted `xi::Record`, its yyjson mutable-doc backing, the
> `DocRegistry` γ-4 cross-ABI refcount, copy-on-write, `share_out`/`adopt_shared`,
> the host `DocChunkPool`, and the `xi_yyjson_abi` load gate. Those mechanisms are
> **gone**; they survive here only as the labeled history at the end
> ([What THE CUT deleted](#what-the-cut-deleted-record-doc-plane), retained so old
> links and log breadcrumbs still resolve). The live contract, fault semantics and
> ingress live in [`pack-plane.md`](./pack-plane.md); this page is the
> design-of-record for the Pack data layer's refcount + storage mechanics.

A **Pack** is a sealed, insertion-ordered list of `(key, tag, value)` entries.
Values are the msgpack scalar/binary tags plus the domain tags: an **image**
(dims + a pool-backed pixel buffer), a **nested-mp** subtree (one canonical
msgpack blob for arrays/maps), and — since the pack-v3 slab — a **tensor**
(logical shape + `PackDtype` over a pool buffer) and typed **user blobs**.
Once sealed a pack is **immutable** — new information is always a new pack.

**Storage model (pack-v3 slab): memory == wire (④A).** In memory a sealed pack
is one contiguous slab (header + hash-sorted directory + insertion-order table +
payload) in which an **inline entry stores its canonical msgpack value** — a
typed read is a directory binary search plus a fixed-width header skip at a known
offset ([`pack-plane.md`](./pack-plane.md) § container memory model). Because the
inline payload IS the wire value, the walk API (`for_each_entry` +
`canonical_value(i, mp::Writer&)`) **splices each inline entry's stored bytes
verbatim** — a copy, not a re-encode (only EXTERN Image/Tensor/large-bin entries
build their wire shape at the edge). Immutability plus that structural identity
is what keeps the *two external* representations one format: the XEX1-v3 frame on
the wire and the `.xex1` file on disk are the same canonical bytes, so record →
replay is byte-lossless with nothing to re-serialize.

## The model: mirror the image pool, for packs

A sealed pack is a C++ container (`xi::Pack`, `xi_pack.hpp`) that **owns one
slab and pool handles**; a plugin in another DLL cannot touch its layout. So — exactly
like an image — a pack crosses the ABI as an **opaque handle** (`xi_pack_handle`)
plus a table of accessor C functions, never as raw struct layout:

| | image | pack |
|---|---|---|
| crosses the ABI as | `uint64` handle | `uint64` handle (`xi_pack_handle`) |
| backing store | host pixel pool (`ImagePool`) | host `PackRegistry` (slab + adopted pool handles) |
| reads / writes | `image_data(handle)` | the `xi_pack_v1` C vtable (spans in / spans out) |
| reclaim | refcount → return slot | refcount → destroy pack, release its pool handles |
| cache across frames | `image_addref` | `retain` (refcount bump, no copy) |

Because both currencies are opaque handles over a host-owned refcounted store,
there is **no serialize on the live path and no shared-mutable state to protect**
— the property the retired doc plane spent the `DocRegistry` + COW + leak-over-UAF
machinery to approximate falls out for free once the payload is sealed.

## The accessor vtable — `xi_pack_v1` (`xi_abi.h`)

Everything a plugin, the script SDK and the host funnel do to a pack goes through
one process-stable C vtable, resolved once via
`host->get_interface("xi.pack", 1)` and cached (`Plugin::pack_iface()`). Its
address and every fn-pointer are stable for the host's life (a Meyers singleton,
`pack_v1_iface()` in `xi_pack_abi.hpp`). Two halves:

- **builder side (produce):** `builder_new` → `builder_add_i64/f64/str/bool/bin/
  image/adopt_image/mp` → `builder_seal` (mints a handle, refcount 1) or
  `builder_abandon`. The SDK sugar is `xi::PackOut` (`xi_abi.hpp`), which owns the
  builder and calls these; a source seals + dispatches with `Plugin::emit()`.
- **accessor side (consume):** `count`, `key_at`/`tag_at` (generic index walk —
  the self-description a producer-agnostic sink enumerates), `tag_of`, and the
  typed reads `get_i64/f64/str/bool/bin/image/mp`. The SDK sugar is `xi::PackIn`
  (plugin door) and `xi::ScriptPack` (`t.pack()`, script side); both return
  `std::optional` so absence is explicit. `retain`/`release` are the refcount, and
  `emit_pack` is the source dispatch verb.

Reads hand back **borrowed spans** into the pack's slab/pool buffers, valid while
the caller's ref on the handle is held. There are no string literals at call sites
in ported plugins: a plugin reads/writes with its schema's key **constants**
(`_keys.gen.h`, see [`typed-io.md`](./typed-io.md)), still drift-proof, but the
resolve across the door is by key **string** through the vtable — there is no
compile-time-offset path (the same-DLL `TypedPack<Schema>` container that
offered one was deleted 2026-07-11, commit `cba51fe`, with zero production
consumers).

### Storage duality (D1)

A binary or image entry lives either **inline in the slab** (small payloads) or
in a **pool buffer** (large / image pixels, adopted by refcount so a source's
painted frame crosses into the pack with no heap→pool copy — `adopt_image`).
The slab side holds a 24-byte `ExtRecord {handle, logical w/h/c, length}` per
pooled entry. The consumer never sees the difference: `get_bin`/`get_image`
resolve both to one borrowed span. Image pixels ride the same zero-copy
`ImagePool` slots the image plane uses, so `get_image` yields a pixel span +
dims a `cv::Mat` can wrap directly. Tensors (`add_tensor`/`get_tensor_of<T>`,
dtype fail-closed) and typed user blobs (`add_blob`) use the same pooled
storage in-process; they have **no `xi_pack_v1` door accessor** — that surface
is reserved for the `xi.pack@3` door (in flight on the packv3 line).

## Refcount + owner-tagged sweep — `PackRegistry` (`xi_pack_abi.hpp`)

The registry is the handle table behind `xi.pack@1`: it maps a handle → a sealed,
**refcounted** `xi::Pack` (and a builder handle → a `PackBuilder` under
construction). A sealed pack is single-owner in C++ but refcounted across the ABI
— an event on the dispatch queue and the emitter can each hold a ref, exactly as
image handles do — so the registry stores each pack with a small refcount and
destroys it (releasing its pool handles) on the last release. Map nodes are
pointer-stable, so a `Pack*` handed to an accessor stays valid across concurrent
insert/erase of *other* handles; the caller holds a ref on its own handle, so that
entry cannot vanish under it.

**Owner-tagged refs (the owner sweep).** This is the pack analogue of
`ImagePool::release_all_for`. The `ImagePool` sweeps leaked image handles per
owner on instance destroy; the registry does the same for sealed packs. Every ref
acquired under an `ImagePool` owner context (a `seal` or `retain` inside the
adapter's `OwnerGuard` around every plugin entry point) is tagged in a small
per-slot ledger; `release_all_for(owner)` drops exactly that owner's outstanding
refs — precisely as if the plugin had called `release()` itself — and returns the
count so teardown can print "swept N leaked pack ref(s)". Framework-internal
transient refs (the dispatch event's ref taken in `f_emit_pack` via
`retain_untagged`, released from another thread with no owner context) are
**untagged** and never swept. The ledger reconciles a release against the caller's
own bucket first, then untagged headroom, then any bucket, so `sum(ledger) ≤ rc`
always holds and a sweep can never free a pack out from under a live co-owner.
`PackRegistry::live_frames()` + `live_builders()` (with `ImagePool::cumulative()`)
are the leak oracle the pack-door tests assert against.

## Fault + provenance ride the pack

There is no separate NA/metadata channel. A contract failure is a **normal sealed
pack** carrying `$fault` (reason code), never `XI_PACK_NULL` (reserved for hard
internal failure); producer identity (`$src`) and the `/`-joined hop chain
(`$prov`) are stamped **before seal** by the producing side, and the host funnel
propagates a fault input by minting a new fault pack (reason + this hop appended)
*without* running the plugin — the pack mirror of the retired
`if (input.is_na()) return Record::na(reason)`. The reserved `$`-keys and the
vtable-level helpers (`is_fault`, `propagate_fault`, `prov_append`) are one home
for all three sides of the seam: `xi/xi_pack_contract.hpp`. Full semantics:
[`pack-plane.md`](./pack-plane.md) and docs/new_gen/15.

## Fallback paths

- **Cross-process / remote / persistence / WS→JS** — the pack's ONE canonical
  msgpack emit (the `canonical_value` walk feeding the XEX1-v3 encoder) is both
  the wire and the disk (`.xex1`) form, so there is no *second* serializer to
  fall back to or drift from: the same bytes travel unchanged between wire and
  disk. (Human-readable JSON remains the currency of the *control* plane —
  `exchange`/config — which is independent of the pack data plane.)

## Tests

The pack plane's leak discipline is asserted by the pack-door tests (registry
`live_frames`/`live_builders` == 0 after teardown, alongside `ImagePool`
cumulative), the owner-sweep tests (`release_all_for` reclaims exactly a dying
owner's refs, never a co-owner's), and the app-team QA references named in the
migration brief: `qa_use_pack_door` (build → chain → push, zero Record),
`qa_pack_record_replay` (record → save → replay, byte-lossless), `qa_pack_order`
(`$seq` + ordering), `qa_pack_fault_path` (fault propagation). Container/registry
detail: [`pack-plane.md`](./pack-plane.md).

---

## What THE CUT deleted (Record / doc plane) — historical

> Everything below is **retired at THE CUT (v12)**. It is kept as a tombstone for
> anyone following an old link or reading a pre-cut log line; none of these
> mechanisms exist in the shipped backend. The pack plane above needs none of
> them, because a sealed pack is immutable and self-contained.

- **`xi::Record`** — a yyjson mutable doc + a named-image map — was the data
  currency. It crossed every in-process plugin boundary by doc **pointer** (not
  serialized), with an intrusive refcount box (`DocBox`) governing same-side
  copies and **copy-on-write** freezing shared copies so the first mutation
  isolated them.
- **`DocRegistry` (γ-4, `xi_doc_registry.hpp`)** — a sharded `doc* → count` map —
  was the host-owned authoritative refcount that let both sides of the ABI hold a
  doc without a copy (the doc analogue of `ImagePool`). The C++ seam was
  `Record::share_out(retain, release)` (enroll + reserve a ref for the adopter)
  and `adopt_shared(release, frozen)` (consume the reserved ref). This whole
  reserve/consume handshake — and its deliberately unbalanced **leak-over-UAF**
  edge on a mid-call plugin crash (leak one host-owned doc rather than risk a
  double-free), the `crash_leaked_docs_lifetime` counter that made that residue
  observable, and the `on_fault` quarantine that bounded it — is gone: a sealed
  pack has no host-reserved ref to strand, so a torn callee leaves nothing the
  host must choose to leak-or-free.
- **The host `DocChunkPool` (`xi_doc_pool.hpp`)** — a thread-local, size-class
  segregated free-list backing `doc_chunk_alloc/realloc/free` so yyjson
  bump-allocated nodes hit the pool a handful of times per doc — is deleted with
  the doc slots it served.
- **The `xi_yyjson_abi()` load gate** — every plugin exported a stamp of its
  vendored yyjson layout, and the host handed a raw doc pointer only on a match
  (else refuse-at-load, or the slow JSON path under `json_fallback`). A pack
  crosses as an opaque handle and needs no layout stamp, so the export and the
  gate are gone (the `xi_yyjson_abi` export was removed from `XI_PLUGIN_IMPL`).
