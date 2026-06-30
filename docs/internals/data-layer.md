# Data layer — yyjson + in-process doc pass-by-pointer + γ-4 refcount

**Shipped design-of-record.** How `xi::Record`'s JSON data crosses between script
and plugin with zero serialization and zero copy, and how either side caches it.

`xi::Record` is a yyjson mutable doc + a named-image map. (cJSON was removed; an
explored MessagePack/CWPack codec was dropped — unknown-count headers + hard
in-place mutation. The kept rationale lives in `docs/archive/` if you need it.)

## The problem it solves

Record crosses every (now in-process) plugin boundary. The naive path serializes
every hop: caller `yyjson_mut_write`s its doc to JSON bytes, callee `yyjson_read`s
them back — twice per `use().process()`. Measured at N=150: cJSON ~1163 µs,
yyjson ~75 µs/direction. The in-process doc-pointer path below skips **all of it**
(0.00 µs — it doesn't serialize at all).

## The model: mirror the image pool, for docs

Images are already cross-DLL-safe because the **host owns one refcounted pool**;
plugins borrow handles. The data layer gives the yyjson doc the same shape:

| | image | doc |
|---|---|---|
| crosses the ABI as | `uint64` handle | `yyjson_mut_doc*` pointer |
| backing store | host pixel pool | host **doc-chunk pool** |
| reclaim | refcount → return slot | refcount → return chunks |
| cache across frames | `image_addref` | refcount bump (no copy) |
| foreign / cross-process | — | fall back to JSON `data`/`len` |

So `xi_record.doc` / `xi_record_out.out_doc` carry a `yyjson_mut_doc*` directly;
`data`/`len` carry JSON bytes only when the doc pointer is null.

**Owner-sweep respects outstanding refs.** Each pool entry is tagged with the
owner (instance) that allocated it, so the pool can reclaim a dead instance's
handles (`ImagePool::release_all_for`, run on destroy / hot-recompile / rename).
Because the *same* handle is shared zero-copy across instances (producer P's
`image_addref` → consumer Q's `adopt_pool_handle`), a caching consumer Q can
legitimately still hold a frame whose owner is P. The sweep therefore drops only
**P's own ref** per entry (exactly like `release()`): a sole-held entry (genuine
leak) is reclaimed immediately, while an entry a live consumer still holds
survives — its owner neutralised to anonymous — and is freed by its last holder.
The sweep never force-frees a still-referenced entry (which would dangle Q's
cached `xi::Image`).

## The host doc-chunk pool (`xi_doc_pool.hpp`)

`DocChunkPool` backs `host_api.doc_chunk_alloc/realloc/free`. A **thread-local,
size-class segregated free-list** (powers of two, 64 B … 64 KiB; oversize →
`malloc`): O(1) acquire/release, no search. yyjson bump-allocates nodes *within*
a chunk, so the pool is hit a handful of times per doc, not per node — **zero
`malloc` per frame after warm-up**. A 16-byte header carries the size class so
`free`/`realloc` (which yyjson calls without a size) can find it.

Cross-DLL: the doc stores these fn pointers in `doc->alc`, so whichever side
drops the last ref, the free routes back to the host pool. Safe under the
project's `/MD` shared CRT — the cross-CRT *free* hazard that forced cJSON to
serialize never applies to *reads*, and frees go through the host.

## Record refcount + copy-on-write (`xi_record.hpp`)

A Record's doc lives in an intrusive refcount box: `DocBox { yyjson_mut_doc* doc;
atomic<int> rc; void(*host_release)(void*); }`.

- **Copy / assign = `rc++`, zero deep-copy.** Both copies are marked **frozen**;
  the first mutation copy-on-writes (`cow_`) into a fresh sole-owned doc. Move
  transfers the box. So same-side caching / fan-out is zero-copy; a held snapshot
  is just the shared frozen doc.
- `host_release` distinguishes a plain locally-owned doc (freed directly with
  `yyjson_mut_doc_free`) from a **registry-managed** one (freed via the host — see
  below). Every Record copy/assign is a refcount bump; there is **no deep copy
  anywhere in the doc layer**.

## Cross-ABI refcount — `DocRegistry` (γ-4, `xi_doc_registry.hpp`)

`DocBox.rc` governs same-side copies (hot, no ABI). To hold a doc on **both sides
of the ABI** without a copy, the host owns the authoritative refcount — the doc
analogue of `ImagePool`. `host_api` gains `doc_retain` / `doc_release` /
`doc_refcount` (ABI v4); `DocRegistry` is a **sharded** `doc* → count` map (16
mutexes by pointer, so parallel emit doesn't contend).

The C++ seam is two methods on `Record`:
- **`share_out(retain, release)`** — enroll this side's doc into the registry and
  **reserve a ref for the adopter**, returning the raw pointer. The reserve is the
  subtle part: the producer's own Record can die *before* the other side adopts
  (a plugin's output Record is destroyed when `process()` returns; the host adopts
  after), so without it the doc would free out from under the adopter.
- **`adopt_shared(release, frozen)`** — the receiving side **consumes** the
  reserved ref (no extra retain) and wraps the doc in a registry-managed box.
  `frozen` comes from `host->doc_refcount`: **sole side → writable** (zero COW,
  write-through intact — the common dispatch case); **shared → frozen** (a plugin
  still caches the doc, so the first mutation COWs to isolate them).

A JSON-fallback target never adopts, so the side that took the JSON branch
(`use_process_cb`) releases the reserve to balance it.

### Both directions symmetric

Input and output both go through `share_out`/`adopt_shared`:
- **Output**: plugin `share_out`s its result; the host adopts it. Plugin can cache
  its own output zero-copy and re-emit.
- **Input**: the host `share_out`s the input doc before the call; the plugin
  adopts it and can **cache the borrowed input across frames zero-copy** — the
  registry keeps it alive until the plugin drops it. (Cost: one retain/release per
  node per dispatch — noise at image-processing ms scale.)

## The load gate (no silent fallback)

Every `XI_PLUGIN_IMPL` plugin exports `xi_yyjson_abi()` = a stamp of
`YYJSON_VERSION_HEX ^ (sizeof(yyjson_mut_doc) << 8) ^ (sizeof(yyjson_mut_val) << 18)`. The host hands
a raw doc pointer **only** when the stamp matches its own. A mismatch (different
vendored yyjson) or a missing export means the plugin can only run the slow JSON
path — so it is **refused at load** with a clear error, unless its `plugin.json`
sets `"json_fallback": true` (then it loads on JSON with a one-shot warning). See
[`../reference/c-abi.md`](../reference/c-abi.md) §4.

## Fallback paths (always correct)

- Layout mismatch / no `xi_yyjson_abi` (opted into `json_fallback`) → serialize to
  `data`/`len`.
- Cross-process / remote (no shared address space) → JSON bytes.
- Persistence, config, WS→JS → JSON text regardless (cold / human-readable).

## Tests

`test_record` (#17 refcount+COW, #18 share/adopt reserve-consume, #19 zero-copy
input cache, #20 1000-round dispatch balance, #21 concurrency), `test_doc_pool`,
`test_doc_registry`, `ws_fallback_gate` (load refusal + opt-in), `ws_cache_input`
(end-to-end real plugin caching its input across frames).
