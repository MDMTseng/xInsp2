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
>   *free* hazard that forced cJSON to serialize never applies to *reads*. Input
>   = host-owned doc; output = plugin-TLS-owned doc; each frees its own.
> - **Serialize (`yyjson_write` → JSON text) only at real boundaries**: WS→JS
>   (the extension/HMI keep `JSON.parse` — no `@msgpack/msgpack`, human-readable),
>   persistence/config, and a fallback for a foreign plugin built against a
>   different yyjson version. So the WS-msgpack work (old option 1) is **moot**.
> - **Memory**: a thread-local `yyjson_alc` **pool** per dispatch thread → near
>   **zero allocation per frame** (vs cJSON's malloc-per-node churn + fragmentation).
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
> `refactor/yyjson-dom`.

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
