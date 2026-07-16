# Pack plane — the uniform keyed-buffer data currency

**Shipped design-of-record (polaris2 waves 1–2 + U1–U3; self-describing blob
plane, spec 30).** The Pack is the data currency: one uniform container —
`key(string) → (type tag, const bytes)` — with **no image/tensor special case**.
Every non-scalar payload is a **self-describing blob**: a pool buffer whose head
describes its own payload, with `xi/image` reduced to a *convention type* carried
in the blob's descriptor. The core owns buffers and dispatch, not images. It runs
**alongside** the Record plane (`data-layer.md`) as a transitional dual carry
until THE CUT; a plugin or script can speak either or both. Decision records:
[`../new_gen/07-uniform-keyed-buffer-plane.md`](../new_gen/07-uniform-keyed-buffer-plane.md)
(the plane) + [`../new_gen/30-self-describing-blob-plane.md`](../new_gen/30-self-describing-blob-plane.md)
(the blob cut, supersedes the @3 image/tensor/type_id surface); canonical
headers: `xi_pack.hpp` (container), `xi_pack_abi.hpp` (host door),
`xi_pack_contract.hpp` (reserved keys + fault contract), `xi_mp.hpp` (codec),
`xi_ingress.hpp` (untrusted edge).

## The container memory model (`xi_pack.hpp`) — the v3 SLAB

**(pack-v3 slab migration, 2026-07 packv3 branch.)** One sealed pack = **one
contiguous slab** + N pool-backed EXTERN buffers, resolved behind one API (D1
"storage duality, API unity"):

- **Slab** — all inline storage in a single heap block, laid out as
  `[PackHeader 64B] [DirEntry × n, 32B, sorted by (key_hash, key bytes,
  ordinal)] [order table: order[ordinal] → dir index] [payload: keys + entry
  bytes, bump-packed, per-entry aligned]`. Lookup is a binary search on the
  hash-sorted directory (equal-hash runs memcmp-verified against the real key
  bytes — collisions handled, never assumed away); **iteration is insertion
  order** via the ordinal→dir order table, so `key_at`/`tag_at`/`for_each`
  present exactly the order entries were added (the walkers' contract). Small
  and large packs share the same O(log n) path — no side index, no hash map,
  no per-entry heap nodes.
- **Inline payloads are canonical msgpack (memory == wire, ④A)** — an inline
  entry stores its *canonical value* verbatim: i64 = int64 `0xd3`+8, f64 =
  float64 `0xcb`+8, bool = `0xc2`/`0xc3`, str = str32, small bin = bin32,
  nested msgpack (`Mp`) its canonical bytes. So `raw_at(i)` **is** the wire
  bytes, and a typed read skips the fixed-width header at a known offset (one
  branch, zero-copy for str/bin). NaN doubles are flattened to the one quiet
  pattern at `add_f64` time (ruling 1, applied by the canonical encoder).
- **EXTERN entries** — every **blob** and every binary ≥ the 4096-byte
  `kPackLargeThreshold` do *not* live in the slab. They are raw ImagePool
  buffers referenced by handle, minted **only** through the typeless
  `pack_pool` facade (a typeless N-byte buffer is an (N,1,1) image) — the
  privileged mint path of doc 07's ingress rule. The slab payload holds a
  **16-byte `ExtRecord {handle, total_len}`** per EXTERN entry; the DirEntry
  points at it. The logical *shape* retired with images/tensors — a blob's
  shape lives in its **descriptor**, not any core struct. `DirEntry::type_id`
  reverted to a reserved-zero field (the dtype-in-type_id / user-blob-type
  encoding is deleted).
- **Recycling** — the slab buffer is borrowed from / returned to the
  per-thread **`SlabPool` freelist** (the previous `ArenaPool`, renamed), and
  the builder's staging vectors recycle through a per-thread scratch pool, so
  a steady stream of packs on one lane's thread is heap-free after warmup —
  the ImagePool discipline in miniature. Destruction returns the slab in one
  shot ("the slab dies with the pack"). Both freelists sit behind a
  **trivially-destructible thread_local pointer** (the same teardown-ordering
  hardening the pixpool magazine carries): a `Pack` legitimately outliving its
  producer thread and dropped inside a *later* thread_local destructor — or
  during static teardown — finds the pointer null and frees its slab outright
  instead of touching a freelist vector that was already destroyed. The one
  recycle pool a dropped pack reaches never becomes a teardown UAF.
- **Self-describing blobs (`PackTag::Blob`, always EXTERN).** The one
  non-scalar entry kind (spec 30). A blob's pool buffer's *head describes its
  own payload*:

  ```
  +0   u32  magic 'XBD1' (0x31444258 LE)      — fail-loud discriminator
  +4   u32  desc_len                           — bytes of descriptor msgpack
  +8   canonical msgpack map (the descriptor, string keys)
  +8+desc_len … zero pad …
  +payload_off = align_up(8 + desc_len, 64)    — payload, 64B-aligned
  +payload_off + payload_len = total buffer length
  ```

  Base is 64B-aligned (pool guarantee) and `payload_off` is a multiple of 64,
  so the **payload is always 64B-aligned** (SIMD / `cv::Mat` wrap / GPU upload).
  The descriptor is a **canonical msgpack map**; the core validates *form* only
  and interprets no key. `"t"` (a namespaced type string, e.g. `"xi/image"`,
  `"acme-scan/profile3d"`) is required **by convention** and read only as sugar
  (`type_of`) — the core owns no type space. Verbs:
  - `mint_blob(desc, desc_len, payload_len) → BufRef` — writes the head, returns
    the buffer with a pointer to the 64B-aligned payload region for **in-place
    fill** (camera DMA lands at `base+payload_off`); RAII (`BufRef` releases its
    mint ref on drop).
  - `adopt_blob(key, BufRef/handle)` — validates the head, addrefs, pack co-owns
    (zero copies).
  - `add_blob(key, desc, desc_len, payload, payload_len)` — mint + copy
    convenience.
  - `get_blob(key) → {desc mp view, payload span, payload_len, handle}`;
    `type_of(key)` reads `"t"`.
  - `blob_head_validate(base, len)` — the ONE fail-loud validation seam (magic,
    `desc_len` bounds, canonical-map descriptor, `payload_off ≤ len`), reused by
    `adopt_blob` and exported for the door/wire packages.
  - Convention helpers on top (core, but interpret nothing): `BlobDesc` (a
    canonical-map writer), `make_image_desc(w,h,c,dt)`, `mint_image(w,h,c,dt)`,
    and `Pack::desc_find_str/desc_find_i64` (the SDK image accessors read
    `w/h/c/dt` through these).

  - **Padded sub-layout inside a payload (`xi_blob_head.hpp`).** A custom type
    often lays its payload out as `[own info][pad][bulk data]` so the bulk region
    is aligned for SIMD / `cv::Mat` wrap / GPU upload. Because the payload *base*
    is already 64B-aligned, `align_up` **within** the payload gives the bulk
    absolute alignment. `padded_layout(head_len, data_len, align=64) → {data_off,
    total}` (overflow-checked, fail-loud) does the offset math, and
    `place_padded_head(payload, head_bytes, layout)` memcpys the head, **zeroes
    the pad gap** (deterministic — the whole payload rides the wire verbatim), and
    returns the aligned bulk pointer. This is **mechanics, not per-type sugar**
    (the spec-30 sugar-boundary ruling): it is convention-neutral — writes no key
    and names nothing — so the caller records `data_off` in its *own* descriptor
    (e.g. `{"t":"acme/scan","data_off":…}`) for a reader to resolve the bulk.

  > **Retired with the blob cut (spec 30):** `PackTag::Image` / `PackTag::Tensor`,
  > `PackDtype` + the dtype-in-`type_id` encoding, `kPackTypeUserBase` + the
  > user-blob type space, `ExtRecord`'s `w/h/c`, and the `add_image` /
  > `adopt_image` / `add_tensor` / `adopt_tensor` / `get_image` / `get_tensor` /
  > `get_tensor_of` / `type_id_of` core verbs. The **`xi.pack@3` door was
  > deleted** (`get_interface("xi.pack",3)` answers NULL forever) and
  > **`xi.pack@4`** (the blob door) replaced it; the frozen `xi.pack@1` image
  > slots are now ~30-line door adapters over the blob rep (package B). Wire
  > gains one blob arm and drops the image arm (package C).

### ImagePool pixel storage — the size-class recycler (`pixpool`)

Where the pool buffers' *bytes* come from (2026-07, hotpath-perf C-1 fix,
backported from the design-C prototype): `ImagePool::create()` no longer heap-
allocates a `std::vector` per image. Pixel storage is a **64-byte-aligned
buffer from a size-class recycler** (`pixpool` in `xi_image_pool.hpp`):

- **2ⁿ size classes, 4 KiB–64 MiB**; a request above 64 MiB (the pool's
  per-image cap is 1 GiB) takes a **direct heap lane** (exact-size
  `_aligned_malloc`/`_aligned_free`, never cached).
- **Free path:** per-thread LIFO **magazine** → mutexed global **overflow
  shelf** → heap. A buffer created on thread A and released on thread B
  migrates to B's magazine. **Byte budgets** (constexpr-tunable): magazine ≤ 4
  buffers/class/thread and ≤ 64 MiB/class/thread (so the 64 MiB class keeps 1);
  shelf ≤ 32 buffers/class and ≤ 128 MiB/class (so the 64 MiB class hoards at
  most 2). Over-budget frees go straight to the heap.
- **No zero-fill (CT ruling 2026-07):** `create()` returns **uninitialised**
  pixels — a recycled buffer carries the previous image's bytes. The canvas
  zero-fill was removed (the info-leak from stale bytes is accepted as
  unimportant), and the `create()`/`create_uninit()` split collapsed to one
  uninitialised mint. **The producer overwrites what it exposes**; a
  partial-paint producer must clear the regions it does not write, or stale
  pixels ride onto the wire / into a record. Two things stay: `pack_pool::alloc_bytes`
  is a copy path where a **null src is a hard reject** (a copy with nothing to
  copy is a caller bug), and the blob **head pad** (between descriptor end and
  payload_off) plus `place_padded_head`'s pad gap are **still zeroed explicitly**
  — that is **wire determinism** (those bytes ride the wire verbatim), not a
  security zero. Recycling stays ~10x cheaper than a fresh alloc + first-touch
  faults for the hot same-size case (1920×1200: ~533 µs → ~53 µs per
  create/release cycle), now with no memset at all.
- **Teardown:** the shelf is intentionally leaked (same doctrine as the
  `ImagePool` singleton), so per-thread magazine destructors can always drain
  survivors to it no matter how late they run.
- Handles, generations, refcounts, owner sweep and `WalkGuard` deferred
  reclamation are **untouched** — only where pixel bytes come from and where
  they go on free changed. Diagnostics: `ImagePool::pixel_alloc_stats()`
  (magazine/shelf hits, evictions); asserted by `test_image_pool_recycle`.

### Memory == wire: canonical msgpack inline (④A, doc 28 finding ④)

The **wire/at-rest format is unchanged** — the canonical max-width msgpack
profile (int64 `0xd3`, float64 `0xcb`, bool `0xc2/0xc3`, str32/bin32, via
`xi_mp.hpp`) remains the serialization truth. For **inline entries the stored
payload IS that canonical value**, so a serialization boundary is a *copy*, not
a transformation — the design's own deciding property (doc 07 "boundaries
become copies, not transformations"), restored after an interim slab state that
stored scalars raw. The walk API:

- `for_each_entry(fn)` / `entry_at(i)` — visit every entry in insertion order
  with a typed `EntryView` (key, tag, inline raw span *or* EXTERN `ext_len` +
  borrowed handle).
- `canonical_value(i, xi::mp::Writer&)` — append the i-th entry's ONE
  canonical msgpack value. For an INLINE entry this is a **verbatim splice of
  `raw_at(i)`** (a copy); a pooled bin is re-wrapped as bin32 at the edge. A
  **Blob** has no single scalar form and returns `false` — its wire arm is the
  self-describing buffer *verbatim* (memory == wire by construction; the wire
  package's contract).

The identity "memory == wire" is pinned by
`plugins/xex1_v2_identity_test.cpp`: **`raw_at(i)` == `canonical_value` == an
independent `xi::mp::Writer` re-encode == the XEX1-v3 encoder's wire bytes ==
disk** — a *structural* identity, not a walker convention. `raw_at(i)` returns
the entry's stored canonical bytes for every inline entry (empty for EXTERN
entries — resolve those with `get_bin`/`get_blob`), and `arena_bytes()` became
`slab_bytes()`.

### One container, one read path

- **`Pack` / `PackBuilder`** — the dynamic, string-keyed container (generic
  walkers, ad-hoc producers, ingress-canonicalized foreign maps). Lookup is
  ONE path for every size: binary search on the hash-sorted slab directory
  (collision runs memcmp-verified; duplicate keys first-inserted-wins) — no
  side index, no per-pack map (the pre-slab hybrid "linear ≤ 24 /
  `unordered_map` at seal" scheme is gone).

> **Retired 2026-07-11 (contraction, commit `cba51fe`):** the second in-process
> container — `TypedPack<Schema>` / `TypedPackBuilder` over a `PackSchema` CRTP
> struct, a compile-time-offset accessor path — was deleted from `xi_pack.hpp`
> (~443 LOC) with zero production consumers. `Pack`/`PackBuilder` is now the only
> in-process container, leaving one container + one codec to audit. The
> *script-side* `ScriptTypedPack<Schema>` (`xi_use.hpp`, key-based over the
> opaque `xi_pack_v1` ABI) is unrelated and survives — see
> [`typed-io.md`](./typed-io.md).

## Seal and identity

A pack under construction (`PackBuilder`) is never shareable; `seal()` is the
one-way flip — it hash-sorts the staged entries and writes the one slab
(directory + order table + payload), producing an immutable `Pack` and
emptying the builder (no double-seal, no write-after-seal). A spent or
moved-from builder holds no staging scratch, so every mutator and a second
`seal()` refuse **structurally** (`spent_()` guard → no-op / empty pack) — not
assert-only: in a release build the guard still holds, so misuse is a
deterministic refusal, never a null-scratch dereference. A sealed `Pack` is
**single-owner and move-only** in C++: its
destruction *is* the whole lifecycle end — the slab returned to the recycle
pool in one shot, every pool handle released exactly once. Borrowed views
(`get_str`/`get_bin`/`get_mp`/`raw_at` spans, `key_at` string_views) stay
valid for the life of the owning Pack — the slab is a stable heap block, so
views survive a Pack move. Drop-on-crash is exactly destruction; there
is no reconciliation and no COW, which is what dissolves the Record plane's
caught-crash leak-over-UAF class (`data-layer.md` §Q0f).

Because a sealed pack is immutable, **new information always means a new pack**
— this is why provenance ($src/$prov) is stamped *before* seal by the producing
side, and why fault propagation mints a fresh pack (see the fault contract
below).

## Crossing the ABI — `xi.pack@1` and the registry (`xi_pack_abi.hpp`)

A plugin in another DLL can never see the container's layout. The pack crosses
as an **opaque `xi_pack_handle`** plus the accessor C functions of `xi_pack_v1`
(spans in / spans out) — resolved once via `host->get_interface("xi.pack", 1)`.
The exact vtable (including the additive bool tail and its growth doctrine) is
documented in [`../reference/c-abi.md`](../reference/c-abi.md). The interim
**`xi.pack@3` supplement** (dtype tensors, user-typed blobs, `adopt_bin`,
ordinal iteration) is **retired by the blob cut** (spec 30): `xi.pack@4` — the
blob door (`blob_mint` / `builder_adopt_blob` / `builder_add_blob` / `get_blob`
+ ordinal `entry_at`) — replaced it, and the frozen `xi.pack@1` image slots are
now thin door adapters that synthesize an `xi/image` descriptor over the blob
representation. The door re-cut (**package B**) has landed; the `xi.pack@4`
vtable is documented in [`../reference/c-abi.md`](../reference/c-abi.md) §6.1b.

Host-side, **`PackRegistry`** is the handle table: a sealed pack is single-owner
in C++ but **refcounted across the ABI** (the dispatch event and the emitter can
each hold a ref, exactly as image handles do). Builder handles map to a
`PackBuilder` under construction; `seal` consumes the builder into the pack
table at refcount 1.

### Owner-tagged refs + the owner sweep

The ImagePool sweeps leaked image handles per owner on instance destroy; the
pack registry has the analogue, on the **single-creator-tag** model (the
counted per-owner ledger's replacement): the only owner-tracked ref is the
**creator's one seal ref** — `seal()` stamps the slot with the sealing
thread's ImagePool owner. Every other ref (a consumer's retain, the dispatch
event's `emit_pack` ref) is an **untracked** `++rc`, a consumer's own
responsibility — never charged to (or swept with) the emitting plugin.
`release_all_for(owner)` (teardown: adapter dtor, script unload) drops **at
most that one seal ref per slot**, and only iff still outstanding (the creator
releasing its own ref clears the tag; a handoff clears it via `untag` without
touching rc) — so a sweep is mathematically incapable of over-releasing: a
pack a consumer still holds survives, a leaked seal ref is reclaimed and
reported ("swept N leaked pack ref(s)"). A consumer-retain leak is DIAGNOSED
(`live_frames()`), never swept — the registry fails toward leak, never toward
UAF. Late teardown needs no liveness guard: the registry singleton is
intentionally leaked (mirroring `ImagePool::instance`), so a static-destruction
retain/release can always touch it.

## The fault + provenance contract (`xi_pack_contract.hpp`)

One home for the reserved `$`-prefixed keys, shared verbatim by host, plugin SDK
and script SDK (everything speaks only the `xi_pack_v1` vtable, so the same
inline code is correct on every side). Implementation of
[`../new_gen/15-pack-fault-semantics.md`](../new_gen/15-pack-fault-semantics.md):

| Key | Meaning |
|---|---|
| `$fault` | str reason code — its *presence* makes the pack a fault (`is_fault`), any type (a mistyped `$fault` still poisons; fail loud, never launder) |
| `$fault_key` / `$fault_detail` | offending key / human detail |
| `$src` | immediate producer (instance name) |
| `$prov` | hop chain, `/`-joined, oldest→newest |
| `$seq` | ordering identity (copied forward on propagation so a fault stays correlatable with its frame) |
| `$channel` | routing channel — the established expose/XEX1 sink-lane convention (`pack_contract::kChannel`; the v12 replacement for the deleted `xi::Record::kChannelKey`). **Copied forward on propagation** (like `$seq`/`$stream`) so a short-circuited fault keeps routing to its display/record lane (round-1 doc 28) |

A **fault is a normal sealed pack** carrying `$fault` — never `XI_PACK_NULL`,
which stays reserved for hard internal failure — so the caller always gets a
pack to route to a verdict. The host funnel **short-circuits** a fault input:
`propagate_fault(fi, in, hop)` mints a new sealed pack (original reason +
`$seq`/`$channel`/`$stream`/`$part`/`$eof` when present, `$src` = this hop, hop
appended to `$prov`) *without running the plugin* — the pack mirror of the
Record path's
`if (in.is_na()) return Record::na(reason).set_src(name)`. By design it carries
no image/bin payload: a poisoned frame's payload is exactly what downstream must
not consume.

Who stamps what: the **pack-door glue** stamps `$src`/`$prov` on every non-empty
door output before seal (explicit `PackOut::src()/prov()` overrides it);
**`emit()`/`emit_pack` stamps nothing** — an emitted pack's entry set is the
producer's contract (`record_replay` re-emits disk dumps byte-identical).
Details for plugin authors:
[`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) § "The pack door".

## Dual-carry dispatch

`TriggerEvent` (`xi_trigger_bus.hpp`) carries **both currencies** during the
transition: the Record payload (image map + refcounted meta doc) *and* an
optional `xi_pack_handle pack` (XI_PACK_NULL for every Record-era event). Two
ingress verbs: `emit(...)` funnels a Record; `emit_pack(source, id, ts, pack)`
stores the sealed handle on the event (consuming the caller-provided event ref)
and extracts no images — the pack *is* the payload. A no-sink drop releases the
pack through the installed releaser (`set_pack_releaser`, wired by
`install_pack_abi()`); ordering keys on `id + arrival_id` identically for both
currencies.

The dispatch worker carries the event's pack into the script through the
`xi_trigger_view` (`view.pack`); the SDK `Trigger` takes its **own** retain, so
`t.pack()` (a `ScriptPack`) stays valid however long the script holds it, while
`t.image()`/`t.meta()` keep serving the Record side — one event, both currencies
delivered.

### Staged pack push / flush (sinks)

The pack plane obeys the same ordered-sink discipline as records
([`dispatch.md`](./dispatch.md) §4):

- `xi::use(sink).push(pack)` on a `"sink": true` target is **staged**, not run
  inline: the host retains the pack onto a staged emit (reusing the dual-carry
  event's pack slot) and flushes it after the inspect inside the arrival-order
  gate, so sink deliveries land in frame order under parallel dispatch. A
  non-sink target is pushed inline (door driven now, ack dropped).
- `xi::use(name).process(pack)` is the request-reply path: a `$fault` input
  short-circuits via `propagate_fault` *before* instance lookup; a sink target
  is refused (rc −5 — request-reply and ordered staging don't mix; feed sinks
  via `push()`).

A sealed pack is immutable, so the host cannot inject `$seq` at flush time (the
Record path stamps it on delivery); a producer stamps it itself before seal:
`b.add_i64("$seq", (int64_t)xi::run_id())` (doc 17 ordering — `xi::run_id()` is
the arrival id of the inspection this thread is computing).

## Ingress — the untrusted edge (`xi_ingress.hpp`)

"Trust inside, prove at the boundary." Internal producers write canonical bytes
by construction (`xi::mp::Writer`, `PackBuilder::add_mp` trusts). **Foreign /
untrusted msgpack** (inbound comms payloads, third-party chunk data, replay
files from disk) must pass through `xi::ingress::canonicalize_entry` — the
three-layer edge: structural validation, normalization to the canonical
max-width profile, optional semantic schema hook — which also refuses forged
pool-handle ext bytes (handles are mintable only by the domain's own allocator).
`canonicalize_into(builder, key, bytes, tag)` composes canonicalize + `add_mp`
into the one-call safe path. The safe path is the only path: no other public
route takes foreign bytes to a pack entry.

`xi_mp.hpp` is the codec underneath: a **Writer** that emits only the canonical
max-width profile (fixed-width scalars, widest container/str/bin markers — 100%
standard msgpack, just never compacted; no ext except the privileged pool-handle
mint), a bounded validating **Reader** that accepts both canonical and foreign
compact input (declared-vs-remaining length checks, depth bound, no trailing
bytes, ext rejected by default), and a read-any → write-canonical
**canonicalizer** in one pass.

## Persistence — XEX1-v3

The wire/disk form of a pack is the XEX1-v3 frame:
`'XEX1' + {v:3, channel, seq, frame:{<key>:[<tag>,<value>], …}}` — each entry a
`[tag, value]` pair, so the stored type is recovered exactly from the on-wire
`XI_PACK_TAG_*`, never guessed by shape (the v2 draft's image-descriptor
ambiguity is unrepresentable). One generic encoder walk
(`plugins/expose/src/xex1_pack_dump.hpp` → `encode_pack_v3`) is shared by every
pack sink (expose wire-push, `record_save` disk dumps); the parser
(`xex1_pack_parse.hpp`) gates magic + version and runs every entry through the
ingress canonicalizer (disk is untrusted). `plugins/record_replay` closes the
loop: it re-emits `.xex1` dumps as sealed packs, and re-dumping reproduces the
file bytes exactly — record → save → replay is byte-lossless. Pre-v3 (tagless
draft) files are refused with a sealed `$fault` pack
([`../new_gen/13-replay-file-migration.md`](../new_gen/13-replay-file-migration.md)).

## Tests

`test_xi_pack` (container + the blob surface: mint/adopt/get round-trip with
64B payload alignment, the `blob_head_validate` rejection matrix, `type_of`,
image-as-convention, duplicate-key, `sort_idx` recycle, `alloc_bytes` null-src
reject), `test_pack_door` (door + registry leak oracle), `test_mp` /
`test_mp_fixtures` (codec + goldens incl. hostile vectors), `test_ingress`,
`test_canonical_xcheck`, `test_cap_plane` (the capability plane rides the same
pack shapes), `record_replay_pack_test` (byte-lossless loop), `bench_pack`.

> The blob cut (spec 30) lands in packages: **A (core)** — `xi_pack.hpp` re-cut +
> blob head/validate seam + SDK/cv descriptor helpers + `test_xi_pack` + the
> `sort_idx`/uint32-seal-guard/`$channel`/zeroinit ride-alongs (done). **B
> (doors)**, **C (wire/XEX1)**, **D (fleet migration)** follow; until B/C/D land,
> the door / script SDK / plugin / `xinsp_backend` build is expected to break
> against the new core (core + `test_xi_pack` + `test_mp` build standalone).

## See also

- [`data-layer.md`](./data-layer.md) — the Record-era doc plane this runs beside.
- [`dispatch.md`](./dispatch.md) — how an emit (either currency) becomes a run.
- [`../reference/c-abi.md`](../reference/c-abi.md) — the exact `xi_pack_v1` /
  capability-plane vtables.
- [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) — authoring a
  bilingual plugin.
