# Data layer: yyjson-only, in-process pass-by-pointer (zero serialize)

How Record/Image data moves between plugins (in-process) and out to the UI,
chosen to kill the cJSON serialization tax and minimize copies.

> ## DECISION (revised 2026-06-14): **yyjson-only**, supersedes the MessagePack plan below
>
> MessagePack via CWPack (Increments A/B, on `master`, ~21×) was explored and is
> being **replaced**. Two CWPack pain points killed it as the in-memory model:
> (1) array/map headers need the **count up front** — incremental build with
> unknown N needs reserve-backpatch; (2) **in-place mutation** of a flat buffer
> is hard (size-changing edits = memmove/rebuild). yyjson has neither problem.
>
> **Final design — one library, `yyjson`, replaces cJSON entirely:**
> - **DOM**: `yyjson_mut_doc` for build (incremental, no count, mutable like a
>   tree) + `yyjson_doc` for read (single contiguous allocation, in-situ parse).
>   Record's API is unchanged; only its backing flips cJSON→yyjson.
> - **In-process (script ↔ plugin): pass the yyjson doc BY POINTER — no
>   serialize, no copy.** Reading a yyjson doc is pure `static inline` struct
>   traversal (no malloc/free), so it is **safe across DLLs** as long as everyone
>   includes the same vendored `yyjson.h` (same struct layout) — the cross-CRT
>   *free* hazard that forced cJSON to serialize never applies to *reads*. The
>   concrete ownership model (host owns the allocator, exactly like the image
>   pool; borrow-for-read; output is host-allocated and adopted by the caller) is
>   spec'd in **§γ — in-process doc pass-by-pointer** below.
> - **Serialize (`yyjson_write` → JSON text) only at real boundaries**: WS→JS
>   (the extension/HMI keep `JSON.parse` — no `@msgpack/msgpack`, human-readable),
>   persistence/config, and a fallback for a foreign plugin built against a
>   different yyjson version. So the WS-msgpack work (old option 1) is **moot**.
> - **Memory**: one host-owned, refcounted **doc-chunk pool** (fixed chunk, O(1)
>   intrusive free-list) backs every crossing/cacheable doc — **zero `malloc` per
>   frame** after warm-up, and **caching = a refcount bump (zero copy)**. Mirrors
>   `ImagePool` exactly; spec'd in §γ. (Supersedes the earlier per-thread bump-pool
>   sketch — a bump pool can't keep individual docs alive past a frame, which
>   caching needs.)
> - **cwpack removed** (it was a temporary 21× bridge on master); **cJSON deleted**.
>
> Bench (still the evidence, `backend/tests/bench_record.cpp`): N=150 round-trip
> cJSON 1205µs / **yyjson 70µs (17×)** / CWPack 57µs. yyjson's 13µs vs CWPack is
> noise; staying JSON + mutable + one-lib + zero-frag-with-pool wins. And
> in-process pass-by-pointer beats *all* of them — it doesn't serialize at all.
>
> Migration plan + status: see [[project_wire_format_msgpack]] memory + the
> Phasing section is superseded by: α vendor yyjson + pool wrapper · β Record DOM
> cJSON→yyjson (Record::Value + xi_types) · γ in-process doc-pointer ABI · δ
> service_main/xi::Json/config → yyjson · ε delete cJSON + cwpack. On branch
> `refactor/yyjson-dom`. **α/β/δ/ε done; γ spec'd below, not yet built.**

---

## γ — in-process doc pass-by-pointer (concrete design)

α–ε made Record yyjson-backed and deleted cJSON, but the in-process `process()`
seam still pays a full round-trip: the caller `yyjson_mut_write`s its doc to JSON
bytes (`xi_record.data/len`), the callee `yyjson_read`s them back. γ removes that
round-trip for same-process, same-yyjson-layout calls by passing the
`yyjson_mut_doc*` directly — and falls back to the JSON bytes otherwise.

### Design principle: one host pool, mirror the image pool exactly

Images are already cross-DLL-safe because **the host owns one refcounted pool**;
plugins only **borrow** (read pixels via a pointer, build output into a host slot
via `image_create`, hold across frames via `image_addref`). γ gives the JSON doc
the **same** mechanism — a single host-owned, refcounted **doc-chunk pool**:

| | image (today) | data / doc (γ) |
|---|---|---|
| crosses the ABI as | `uint64` handle | `yyjson_mut_doc*` pointer |
| backing store | one host pixel pool | one host **doc-chunk pool** |
| reclamation | refcount → return slot | refcount → return chunk(s) |
| callee on **input** | borrow-read pixels | borrow-**read** doc nodes |
| callee on **output** | write into a host slot | build into a host-pool doc |
| **cache / re-emit** | `image_addref`, hold | refcount bump, hold (no copy) |
| cross-process / mismatch | serialize / handle | fall back to JSON `data/len` |

There is **no "ephemeral vs retained" split and no per-frame bump pool** — every
doc that crosses (or might be cached) lives in the one host pool; lifetime is
purely its refcount. A doc that dies this frame just drops to 0 quickly; a cached
one is held by extra refs. This keeps images and docs structurally identical and
makes **caching a zero-copy refcount bump** (see Retention below).

### The host doc pool

A single host-owned allocator behind every crossing doc:

- **Fixed-size chunks + an intrusive singly-linked free-list → O(1) acquire/
  release, no search, no per-chunk metadata** (a free chunk stores the next-free
  pointer in its own bytes; acquire = pop head, release = push head). yyjson
  bump-allocates *nodes within* a chunk, so the pool is hit only **per chunk**
  (a handful per doc), not per node — the free-list cost is amortized to noise and
  there is **zero `malloc` per frame** after warm-up, same as the old bump idea.
- **Refcounted**: `yyjson_mut_doc_free` (via the doc's `alc.free`) returns the
  doc's chunks to the free-list; the last ref to drop triggers it. Whoever drops
  it, the free routes back to the host pool → **cross-DLL free is automatically
  safe** (the hazard that forced cJSON to serialize never fires).
- **Thread-safety the same way `ImagePool` already does it**: shard the free-list
  (or give each dispatch thread a thread-local chunk cache that batch-refills from
  the shared pool). No new concurrency problem.
- v1 takes the **fixed-chunk shortcut** (uniform free-list, ImagePool-simple);
  size-classed free-lists are a later packing optimisation, still O(1). Reads
  never allocate (`yyjson_obj_get`/`get_str`/`arr_foreach` are pure pointer
  walks), so the pool is touched only on build/free, never on read.

### Record = refcounted doc + refcounted images; single-writer

Record becomes a handle pair: a **refcounted `yyjson_mut_doc`** (host pool) + the
existing **refcounted image handles**. The ownership invariant (confirmed
requirement — only the originator writes):

> **Single-writer / freeze-on-publish.** A doc has one owner who may mutate it
> *only while it is unshared* (refcount 1, not yet borrowed out). The moment it is
> borrowed for read or its refcount rises (cached / fanned-out), it is **frozen —
> read-only for everyone**. To change a frozen/shared doc, **copy-on-write**:
> `mut_copy` into a fresh (host-pool) doc you own, edit that.

This is immutable-snapshot discipline, and it preserves the old "copies are cheap
until you mutate" semantic — at the API level a borrowed-input Record is a read
view; the first `.set()` transparently COWs into an owned doc, so plugin authors
never see the rule.

### ABI changes (append-only)

`xi_record`/`xi_record_out` each gain ONE optional doc pointer; when it is
non-null the JSON `data/len` is empty (never both, or we serialized for nothing).

```c
typedef struct {
    const xi_record_image* images;
    int32_t                image_count;
    const uint8_t*         data;   /* JSON bytes — used iff doc == NULL */
    int32_t                len;
    const void*            doc;    /* yyjson_mut_doc*, BORROWED read-only (frozen);
                                      non-null only in-process + layout match */
} xi_record;

typedef struct {
    xi_record_image* images; int32_t image_count; int32_t image_capacity;
    const uint8_t*   data;   int32_t len;   /* JSON bytes — used iff out_doc == NULL */
    void*            out_doc; /* yyjson_mut_doc* built in the HOST doc pool;
                                ref handed to the caller (refcount, not deep copy) */
} xi_record_out;
```

`xi_host_api` exposes the doc pool as a yyjson-agnostic allocator (the plugin
wraps these into a `yyjson_alc`); the backing store is the host doc-chunk pool,
not raw `malloc`:

```c
/* Host doc-chunk pool. A doc built through these is host-owned, so any side may
   drop the last ref and the free returns chunks to the host pool — mirrors
   image_create / image_addref / image_release for pixels. */
void* (*doc_chunk_alloc)(size_t);
void* (*doc_chunk_realloc)(void*, size_t);
void  (*doc_chunk_free)(void*);
```

Plus a **layout-compatibility gate** so a foreign prebuilt DLL carrying a
different yyjson is never handed a raw doc pointer:

```c
/* Exported by every plugin built against the in-tree yyjson. Stamp = yyjson
   version + sizeof(yyjson_mut_doc) + sizeof(yyjson_mut_val). Host passes a doc
   pointer ONLY if stamp == host's; else serialize to data/len. Absent → assume
   incompatible → fall back. */
uint32_t xi_yyjson_abi(void);
```

### Flow (in-process, compatible)

1. **Caller** sets `in.doc = record.doc()` (its frozen doc), `in.data/len` empty.
   Images marshalled as handles, unchanged.
2. **Callee** sees `in.doc != NULL` → wraps it as a **read-only Record view** (no
   parse, no copy). To extend/modify, it COWs (`mut_copy`) into its own host-pool
   doc first — read of the frozen input is safe; the copy allocates on its side.
3. **Callee** builds its output Record in the **host doc pool** (its `xi::Record`
   uses the host doc allocator in-process — the doc analog of `out.image(...)`
   landing in the host pixel pool). Returns `out.out_doc = out.doc()`.
4. **Caller** **adopts the ref** to `out.out_doc` as the result Record's doc —
   zero copy, zero parse — and drops it when that Record dies (→ chunks return to
   the host pool).

### Retention & caching (the payoff)

Because every crossing doc already lives in the host pool and is refcounted,
**caching a Record is a refcount bump — zero copy, no "promotion"**:

- **Input revisit / cache N read-only records** (e.g. 30): hold an extra ref to
  each incoming Record's (frozen) doc + `image_addref` its handles; drop refs to
  evict. Reads are free. No deep copy — the snapshot is the shared frozen doc.
- **Image-source cache re-emit**: the emitter holds `{frozen doc ref + addref'd
  image handles}` and re-emits by handing borrows again (cross-process → serialize).
  This upgrades `xi_resource_store.hpp`'s metadata from a JSON string to a retained
  doc ref; the store's shape is unchanged.
- **Leaf / branch borrow**: borrowing a sub-node (`yyjson_val*`) for a *transient
  read* is free (a pointer into the owner's doc). **Retaining** a branch is not a
  thing — yyjson nodes aren't individually refcounted/freeable, their lifetime is
  the whole doc's — so to keep a branch you `yyjson_mut_val_mut_copy` it into your
  own doc. Branch-borrow is a read convenience, not a retention mechanism.

### Ownership & lifetime rule

A borrowed/shared doc is **frozen → read-only**. Reads never allocate and are
trivially safe across DLLs. To change anything you don't solely own, **COW** into
your own host-pool doc. A `mut_copy` is a node-tree walk + chunk-pool allocs (no
string encode/decode), so it is far cheaper than the serialize round-trip it
replaces — and it only happens on the rarer pass-through-modify path; the common
read-input-build-fresh-output path copies nothing.

### Fallback paths (unchanged, always correct)

- `xi_yyjson_abi()` absent/mismatched → host serializes to `data/len`; callee
  `yyjson_read`s. (Foreign third-party plugin, or future ABI skew.)
- Cross-process / remote / isolation (no shared address space) → doc pointer is
  meaningless → JSON bytes.
- Persistence, config, WS→JS stay JSON text regardless (cold / human-readable /
  `JSON.parse`).

### Blast radius

- `xi_abi.h`: `xi_record.doc`, `xi_record_out.out_doc`, the three `doc_chunk_*`
  allocator fns, `xi_yyjson_abi` export. Append-only; bump `XI_ABI_VERSION`.
- `xi_image_pool.hpp` (or a sibling `xi_doc_pool.hpp`): the host doc-chunk pool
  (fixed chunk, sharded free-list, refcount) + wire it into `make_host_api`.
- `xi_record.hpp`: Record = refcounted host-pool doc + image handles; build via
  the host doc allocator in-process; `doc()` / `from_doc(borrowed, frozen)` view
  ctor; freeze-on-publish + COW-on-first-write.
- `xi_abi.hpp` `record_to_c`/`record_from_c`, `xi_use.hpp` `UseProxy::process`,
  `xi_plugin_handle.hpp` `process()`: emit/adopt `doc` when compatible, else JSON.
- `XI_PLUGIN_IMPL`: export `xi_yyjson_abi`.
- **Unchanged:** plugin/script source, io.hpp, nominal types, the JSON wire to JS.

### Open questions / risks

- **In-process allocator binding.** `xi::Record out;` must pick up the host doc
  pool when it's built for an in-process return. A TLS "current host doc alc" set
  around each call is clean for `PluginHandle`/`UseProxy`; a Record built on a
  *different* thread (async worker) than the call wouldn't inherit it → that
  output just isn't host-pool-backed → falls back to serialize for that return.
  Correctness preserved; measure how often it hits.
- **Ref handoff at the ABI.** Output `out_doc` transfers a *ref* to the caller;
  the callee's Record must not also drop it. Encode in `record_to_c` (release-on-
  fill). With a shared refcount the "who frees" question is moot — last ref wins.
- **Layout stamp granularity.** version + two `sizeof`s catches common skew; a
  field reorder within the same version+sizes would slip through. We pin one
  yyjson, so it's belt-and-suspenders — document the assumption.
- **Pool sizing / high-water mark.** The free-list grows to the peak count of
  concurrently-live docs (incl. caches) and stays there (chunks not returned to
  the OS). Caching raises that peak by design; cap chunk size and, if needed,
  trim idle chunks on a low-water timer.
- **Worth it for tiny docs?** The win scales with node count; small metadata may
  not beat the bookkeeping. Same path either way (fallback = today's serialize) so
  it's never a regression — measure on the toolbox pipeline before tuning.

---

_The MessagePack design below is retained as the explored-and-superseded record._

## Why (measured)

Record crosses every plugin boundary as a serialized blob. With everything now
in-process (SHM/worker gone), `UseProxy::process` still does

## Why (measured)

Record crosses every plugin boundary as a serialized blob. With everything now
in-process (SHM/worker gone), `UseProxy::process` still does
`data_json()` [cJSON print] → ABI → `cJSON_Parse`, twice per `use().process()`.
cJSON is the slow part. `backend/tests/bench_record.cpp` (matcher payload, N
matches, min-of-batches, µs):

| N=150       | serialize | parse  | round-trip | bytes |
|-------------|-----------|--------|------------|-------|
| cJSON       | 753.87    | 450.89 | 1204.76    | 15787 |
| yyjson      | 42.65     | 27.12  | 69.77      | 15807 |
| MPack       | 41.16     | 53.74  | 94.90      | 13586 |
| **CWPack**  | **32.60** | **24.29** | **56.89** | 13586 |

cJSON is an outlier — any modern lib is 13–21×. **CWPack is the fastest** (21×),
smallest, and MessagePack's ext/bin types give the extensibility we want.

## Decision

**One format end-to-end: MessagePack, encoded with CWPack.**

- **Internal (plugin ABI, in-process):** the Record payload is CWPack bytes, not
  a JSON string. Hot path (every frame × every stage), C++↔C++, never read by a
  human → pure-speed binary.
- **External (WebSocket → VS Code extension / HMI):** the same MessagePack. The
  JS side decodes with `@msgpack/msgpack` → a plain JS object, identical
  ergonomics to `JSON.parse`. The WS already carries binary frames (images), so
  this rides an existing binary channel.
- **JSON is debug-only:** a flag makes the backend emit JSON (via yyjson, 17×
  cJSON) for eyeballing in DevTools or a third-party JSON client. Not the
  default, not the hot path.

**End-state: cJSON is deleted.** After the wire (CWPack) and the Record DOM
(Phase 2) no longer use it, cJSON's only remaining jobs are *JSON text* — cold
ones: config files (`project.json`/`plugin.json`/`instance.json`/`cert.json`,
which stay human-editable JSON), the `xi::Json` plugin helper
(`exchange`/`get_def`/`set_def`), and the JSON debug dump. All of those move to
**yyjson** (already vendored, 17× cJSON, build+parse both directions). Net libs:
**CWPack (binary) + yyjson (JSON text)**; cJSON removed in Phase 3. "Deleting
cJSON" means swapping those parsers — JSON config files stay JSON.

## The copy-minimizing insight (read side)

The bench's CWPack `parse` (24µs) is a **streaming walk — it builds no DOM**. If
decode instead rebuilds a cJSON tree, the malloc-per-node tree-build cost
(≈ `cJSON_Duplicate`, ~250µs at N=150) stays, so decode is only ~1.6× — most of
the win is lost. Therefore:

> **encode → CWPack is a clean 21×. decode is only fast (and copy-free) if you
> do NOT re-materialize a tree — you read fields directly from the msgpack
> buffer (a view).**

This is what "minimize data copy" requires: the read side must be a view over
the bytes, not a rebuilt tree.

## Resource model — all RAII, shared, minimal copy

Three resources, each single-ownership RAII; passing = refcount bump, not copy:

1. **Image** — already host-owned refcounted handle + RAII (`xi::Image`'s
   `shared_ptr` deleter calls `host->image_release`). Zero pixel copy. Inside a
   Record's msgpack it is referenced by **handle (ext type)** internally;
   materialized as **inline `bin`** only when actually sent over WS — no base64
   (JSON forced base64 = +33% size + encode CPU; the old shape-matcher base64'd
   PNGs into JSON). Unchanged on the build side.

2. **MessagePack buffer** — a RAII, refcounted (`shared_ptr<std::vector<byte>>`)
   owned buffer. The producing plugin encodes **once**; the same buffer is
   handed to the next stage AND forwarded to WS with **no re-encode** (fan-out =
   shared_ptr copy). No TLS-string hack, no cross-CRT free (one owner frees it).

3. **Record** — RAII over one of:
   - a **build buffer** (CWPack writer) while a plugin constructs its output
     (`set`/`push` append into the buffer — build-once, the common case), or
   - a **read view** over a received buffer (`get_*` / `Record::Value` read
     straight off the bytes — zero-copy), or
   - (rare) a mutable DOM, only if a stage mutates a record in place.

   The plugin/script **source API is unchanged** (`out.set("x",5)`,
   `in["k"].as_double()`); only Record's internals change. io.hpp extractors /
   constructors and the nominal types (built on `Record::Value`) ride on top
   unchanged.

Dataflow fit: plugins **build once, read many** — output is constructed and
returned (append into a buffer, no tree); input is read (view, no copy). That
maps cleanly onto write-buffer / read-view. In-place mutation is the only
awkward case (decode-to-mutable if ever needed); it's rare in the pipeline.

## Phasing (pick before building)

**Phase 1 — codec swap (low risk, partial win).** Keep cJSON as the in-memory
DOM; replace only the wire codec. Split for safety:
- **1a — internal plugin ABI:** `record_to_c`/`record_from_c`/`UseProxy` encode/
  decode CWPack; `xi_record` carries `bytes+len` not `char* json`. Self-contained
  in the C++ backend; regression-checked by the existing `ws_*` tests (they
  observe vars over WS, which is unchanged) + `bench_record`. **No JS change.**
- **1b — WS boundary:** service_main WS messages → MessagePack frames; extension
  + HMI decode with `@msgpack/msgpack`. Touches the TS/JS side; separate.

Gets the **21× on encode** immediately and a modest decode win (still rebuilds
cJSON). Smallest blast radius — Record DOM, plugins, io.hpp untouched. Start 1a.

**Phase 2 — msgpack-native Record (full min-copy).** Record becomes a RAII
refcounted msgpack buffer + read-view; build = CWPack writer, read = buffer
view; no cJSON tree on the hot path. Realizes the **full 21× both ways +
zero-copy fan-out**. Bigger: Record reimplemented (`Record::Value` over a
msgpack view, `set`/`push` over a writer); plugins recompile but keep source;
cJSON kept only for JSON debug. The nominal types / io.hpp must keep working over
the new view.

Recommended: ship Phase 1 first (fast, safe, validates the format + the JS
decode), then Phase 2 once the toolbox pipeline gives end-to-end numbers.

## Blast radius

- C ABI: `xi_record` / `xi_record_out` payload `char* json` → `const uint8_t*
  bytes, int32_t len` (+ images already handles).
- `backend/include/xi/xi_abi.hpp`: `record_to_c` / `record_from_c` → CWPack.
- `backend/include/xi/xi_use.hpp`: `UseProxy::process` → CWPack.
- WS server (service_main): structured messages → MessagePack frames; image
  payloads → inline `bin`. JSON-debug flag.
- `vscode-extension` + `hmi`: WS decode → `@msgpack/msgpack`.
- Phase 2 only: `xi_record.hpp` Record internals; `Record::Value`.
- **Stable across both phases:** plugin/script source, io.hpp pattern, nominal
  types ([`io-types-and-na.md`](./io-types-and-na.md)).

## Open questions

- Phase-2 in-place mutation: decode-to-mutable, or forbid (build-once contract)?
- WS: one MessagePack frame per message, or keep separate binary image frames +
  msgpack metadata? (Inline `bin` vs side-channel handle.)
- Phase 3 (cJSON deletion) ordering: migrate config / `xi::Json` / debug to
  yyjson, then remove cJSON. Cold path — do it last.
- CWPack vendoring: bench copy lives in `backend/tests/serde_vendor/`; the
  shipping copy moves to `backend/vendor/cwpack/` (pinned + `cwpack_config.h`).
