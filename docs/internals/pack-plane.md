# Pack plane — the v3 uniform keyed-buffer data currency

**Shipped design-of-record (polaris2 waves 1–2 + U1–U3).** The Pack is the v3
data currency: one uniform container — `key(string) → (type tag, const bytes)` —
with no image/metadata split (an image is just an entry whose tag says "image"
and whose pixels live in a pool buffer). It runs **alongside** the Record plane
(`data-layer.md`) as a transitional dual carry until THE CUT; a plugin or script
can speak either or both. Decision record:
[`../new_gen/07-uniform-keyed-buffer-plane.md`](../new_gen/07-uniform-keyed-buffer-plane.md);
canonical headers: `xi_pack.hpp` (container), `xi_pack_abi.hpp` (host door),
`xi_pack_contract.hpp` (reserved keys + fault contract), `xi_mp.hpp` (codec),
`xi_ingress.hpp` (untrusted edge).

## The container memory model (`xi_pack.hpp`)

A pack's storage has two planes, resolved behind one API (D1 "storage duality,
API unity"). **Since the pack-v3 slab migration (2026-07)** the small plane is
a SLAB, not the old chunked arena:

- **Slab** — one contiguous, recycled buffer per sealed pack:
  `PackHeader(64B) + DirEntry(32B)×n sorted by (key_hash, key, ordinal) +
  ordinal→dir order table + bump-packed payload` (keys + entry bytes). It owns
  every *small* entry's bytes (scalars, strings, inline binaries below the
  4096-byte `kPackLargeThreshold`, nested msgpack) **and** the interned key
  strings. Scalars are stored **RAW** (i64/f64 = 8 aligned bytes, bool = one
  0/1 byte — a read is one load, zero decode); nested msgpack (`Mp`) rides as
  its canonical bytes verbatim. Lookup is a binary search on the hash-sorted
  directory (equal-hash runs memcmp-verified); **insertion order is
  first-class** via the ordinal table (`key_at`/`for_each` — the walkers'
  contract). Slab buffers recycle through a thread-local `SlabPool` freelist
  (the old `ArenaPool`, honestly renamed) and builder staging through a
  per-thread scratch pool, so a steady stream of packs on one lane's thread is
  heap-free after warmup — the ImagePool discipline in miniature. Destruction
  returns the slab in one shot ("the slab dies with the pack").
- **Pool buffers** — large payloads (images; tensors; binaries ≥ threshold) do
  *not* live in the slab. They are raw ImagePool buffers referenced by handle
  (the slab holds a 24-byte `ExtRecord` — handle + logical shape + length —
  per extern entry), minted **only** through the typeless `pack_pool` facade
  (a typeless N-byte buffer is an (N,1,1) image) — the privileged mint path of
  doc 07's ingress rule.

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
- **Zero-fill contract unchanged:** `create()` still returns zeroed pixels —
  recycled buffers are `memset` (callers exist that paint onto a "blank"
  canvas). Still ~10x cheaper than the old alloc + zero + first-touch-fault
  path for the hot same-size case (1920×1200: ~533 µs → ~53 µs per
  create/release cycle; magazine hit itself is nanoseconds).
- **Teardown:** the shelf is intentionally leaked (same doctrine as the
  `ImagePool` singleton), so per-thread magazine destructors can always drain
  survivors to it no matter how late they run.
- Handles, generations, refcounts, owner sweep and `WalkGuard` deferred
  reclamation are **untouched** — only where pixel bytes come from and where
  they go on free changed. Diagnostics: `ImagePool::pixel_alloc_stats()`
  (magazine/shelf hits, evictions); asserted by `test_image_pool_recycle`.

**Wire ≠ memory since the slab (by design), one canonical seam.** Scalars live
raw in the slab; the **canonical max-width msgpack encoding** (int64 `0xd3`,
float64 `0xcb`, bool `0xc2/0xc3`, str32/bin32) is re-emitted at the WALK seam —
`Pack::for_each_entry` / `Pack::canonical_value(i, mp::Writer&)` — through
`xi::mp::Writer`, the one canonical-encode truth (ruling-1 NaN flatten
included, applied already at `add_f64`). A generic dumper (XEX1-v3,
`record_save`) splices those walk bytes onto the wire; they are byte-identical
to what the old arena stored, so the wire/at-rest format is UNCHANGED.
`raw_at(i)` now returns the raw stored payload (a fast in-process read, not
wire bytes).

### One container, one read path

- **`Pack` / `PackBuilder`** — the dynamic, string-keyed container (generic
  walkers, ad-hoc producers, ingress-canonicalized foreign maps). Lookup is
  ONE path for every size: binary search on the hash-sorted slab directory
  (collision runs memcmp-verified; duplicate keys first-inserted-wins) — no
  side index, no per-pack map.

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
one-way flip — it moves the arena, entry table and handle ledger into an
immutable `Pack` and empties the builder (no double-seal, no write-after-seal;
asserts guard both). A sealed `Pack` is **single-owner and move-only** in C++:
its destruction *is* the whole lifecycle end — arena freed in one shot, every
pool handle released exactly once. Drop-on-crash is exactly destruction; there
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
documented in [`../reference/c-abi.md`](../reference/c-abi.md). Since the
pack-v3 slab migration the **`xi.pack@3` supplement** (`xi_pack_v3`, resolved
alongside @1 via `get_interface("xi.pack", 3)`) surfaces what v1's shape could
not: dtype-aware tensor entries, user-typed blobs (`type_id`), zero-copy
`adopt_bin`/`adopt_tensor`, and ordinal-explicit iteration
(`type_id_at`/`entry_at`) — see c-abi.md §6.1b.

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
| `$channel` | routing channel — the established expose/XEX1 sink-lane convention (`pack_contract::kChannel`; the v12 replacement for the deleted `xi::Record::kChannelKey`) |

A **fault is a normal sealed pack** carrying `$fault` — never `XI_PACK_NULL`,
which stays reserved for hard internal failure — so the caller always gets a
pack to route to a verdict. The host funnel **short-circuits** a fault input:
`propagate_fault(fi, in, hop)` mints a new sealed pack (original reason +
`$seq`, `$src` = this hop, hop appended to `$prov`) *without running the
plugin* — the pack mirror of the Record path's
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

`test_xi_pack` (container/arena/typed paths), `test_pack_door` (door + registry
leak oracle), `test_mp` / `test_mp_fixtures` (codec + `protocol/fixtures`
goldens incl. hostile vectors), `test_ingress`, `test_canonical_xcheck`,
`test_cap_plane` (the capability plane rides the same pack shapes),
`record_replay_pack_test` (byte-lossless loop), `bench_pack`.

## See also

- [`data-layer.md`](./data-layer.md) — the Record-era doc plane this runs beside.
- [`dispatch.md`](./dispatch.md) — how an emit (either currency) becomes a run.
- [`../reference/c-abi.md`](../reference/c-abi.md) — the exact `xi_pack_v1` /
  capability-plane vtables.
- [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) — authoring a
  bilingual plugin.
