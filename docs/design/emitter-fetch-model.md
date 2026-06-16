# Emit / fetch dispatch model

How a source plugin hands a frame-set to the run that inspects it — and how that
run reads it back — without threading the data through the trigger bus or guessing
which run corresponds to which frame. Status: **shipped** (resource store +
id dispatch + fetch-by-id; `xi_emitter.hpp` helper).

## The problem

The script is the composition layer: plugins don't call plugins, the script wires
one instance's output into the next instance's input (see
[`io-types-and-na.md`](io-types-and-na.md)). For the normal capture path the
**trigger bus** already does this — a source `emit_trigger`s frames tagged with a
`tid`, the bus *correlates* frames sharing that `tid` (e.g. synchronized
multi-camera) and dispatches the script once per complete trigger.

But correlation is the wrong tool when a single emitter already has a complete
frame-set in hand and just wants to *run an inspection on it*, addressably:

- there is nothing to correlate — it's one emitter, one frame-set;
- the run that processes the frame must be able to name it back (for replay, for
  hot-param re-runs, for a downstream PLC send that needs **gap-free ordering**);
- when the pipeline can't keep up, the *emitter* — not the core — must own the
  drop decision, so it can drop **before** consuming an ordering number and keep
  the downstream sequence contiguous.

## The model

Three moves, all keyed on one opaque **`res_id`**:

1. **Stage.** The emitter calls `emit_resource(emitter_name, res_id, images, n,
   cjson)` — it parks a frame-set (named images + a JSON metadata blob) under
   `res_id` in a per-emitter ring (`ResourceStore`, `xi_resource_store.hpp`).
   The store addrefs each image handle, so the caller may release immediately.

2. **Dispatch.** The emitter calls `emit_dispatch(emitter_name, res_id,
   timestamp_us)`. The host builds an **id-only** `TriggerEvent` (no images),
   routes it by the emitter's *group*, and runs it through the lane / one-shot
   path — **bypassing the bus's per-tid correlation entirely** (`service_main.cpp`,
   `set_dispatch_sink` → `enqueue_dispatch_`). The run carries `res_id` as its
   trigger id.

3. **Fetch.** Inside that run the script reads the id back via
   `xi::current_trigger().id_string()` (the trigger id's hex form) and pulls the
   staged frame with `xi::use(emitter).fetch(id)` → an `xi::Resource`:

   ```cpp
   auto r = xi::use("cam").fetch(xi::current_trigger().id_string());
   if (r.ok()) {
       xi::Image left = r.image("cam_left");   // lazy, addref'd pool view
       // int seq = parse r.data() for "seq"
   }
   ```

   Metadata (`r.data()`, the cJSON) is fetched eagerly; images are fetched lazily
   by key, so a consumer only pays for the images it actually reads.

`xi_emitter.hpp` wraps steps 1–2 (mint `res_id`, assign a contiguous `seq`, stage,
emit, dispatch) behind `xi::Emitter` so source authors get the contiguity contract
right by default:

```cpp
em_.bind(host(), name());
em_.image("img", frame);
if (!em_.emit())  { /* back-pressure — see below */ }
```

## Identity vs. order

`res_id` is the **dispatch + fetch key** — opaque, unique per live frame
(`Emitter` uses the `seq`'s hex; a plugin may use a UUID). The store uses it only
for addressing and replay; it never reads inside it.

**Ordering is separate.** A downstream serialization point (e.g. a PLC send that
must see every frame in order) reads a `seq` field carried **inside the cJSON
metadata**, assigned by the emitter — not from `res_id`. `xi::Emitter` auto-injects
`{"seq":N, ...}` into each frame's `dataInfo`. The store neither reads nor needs
`seq`; identity (where) and order (when) are deliberately different axes.

## Back-pressure contract

`emit_dispatch` returns **1 = accepted** (enqueued / dispatched) or **0 = lane full
or dispatch not running**. The lane is **back-pressure, never silent drop**:
`enqueue_dispatch_` *rejects* when the lane queue is full (unlike the bus path,
which applies drop-oldest/newest/block policy). The id-only event owns no images,
so a reject leaks nothing.

On a `0`, the **emitter owns the choice**:

- **skip-before-burning-a-seq** — drop the frame *at the source*, do **not** consume
  its `seq`. `xi::Emitter::emit()` does exactly this: on a reject it keeps `seq_`,
  clears the staged frame, and reuses the same `res_id`+`seq` next time
  (overwriting the orphaned ring entry). The dropped frame therefore never enters
  the seq stream, so the downstream sequence stays **gap-free**; or
- **retry** the same `res_id` later.

This is the whole point of pushing the drop decision out of the core: a gap-free
`seq` stream downstream is only possible if the thing that *assigns* `seq` is the
thing that *decides to drop*.

## The ring and lifetime

`ResourceStore` keeps a bounded **per-emitter-name ring** of the most recent
`res_id`s (default capacity 16, `set_capacity`). Past capacity the oldest entry is
evicted; re-emitting an existing `res_id` overwrites in place. The ring doubles as
the emitter's recent-frame buffer — a hot-param re-run reads the latest staged
frame straight back out.

Image-handle lifetime mirrors the trigger bus:

- `emit_resource` **addrefs** every retained handle (caller may release right
  after);
- eviction, overwrite, and `clear()` (project close / shutdown) **release** them;
- `fetch_image` returns an **addref'd** handle that the consumer releases (the
  `xi::Resource::image()` / `Image::adopt_pool_handle` path does this for you).

So the store never frees a handle out from under a live consumer and never leaks
one. Metadata is just a JSON string copied into the entry.

## ABI surface

All four live in `xi_host_api` (`xi_abi.h`), added in **ABI v2**, additive at the
struct tail — pre-v2 hosts leave them null (always null-check). Wired by
`install_resource_hooks()` in `PluginManager::default_host_api()`.

| Function | Signature | Semantics |
|---|---|---|
| `emit_resource` | `void(const char* emitter, const char* res_id, const xi_record_image* images, int32_t n, const char* cjson)` | Stage a frame-set under `res_id`. Addrefs each handle (caller may release after). Overwrites a same-`res_id` entry; evicts the oldest past ring capacity. |
| `emit_dispatch` | `int32_t(const char* emitter, xi_trigger_id res_id, int64_t timestamp_us)` | Drive one inspection for a staged resource: id-only event, routed by the emitter's group, **bypassing bus correlation**. `res_id` rides as the run's trigger id (its hex = the fetch key). Returns **1** accepted, **0** lane full / not running (back-pressure — caller decides). `timestamp_us` 0 = host's current time. |
| `fetch_resource` | `int32_t(const char* emitter, const char* res_id, char* cjson_buf, int32_t buflen)` | Read the metadata cJSON. Returns byte length `L` (written iff `L <= buflen`; else resize to `L` and retry), or **-1** if `res_id` isn't staged. Does **not** touch image refcounts. |
| `fetch_image` | `xi_image_handle(const char* emitter, const char* res_id, const char* key)` | Lazily pull one staged image by key. Returns an **addref'd** handle (caller `image_release`s) or `XI_IMAGE_NULL` if the resource or key is absent. Default key `""` = the single-image convention. |

Script-side these are reached through `xi::use(emitter).fetch(res_id)` → `Resource`
(`xi_use.hpp`); host-side they're backed by `ResourceStore` and, for
`emit_dispatch`, the `DispatchSink` that `service_main` installs (until installed —
e.g. the headless runner with no lane pool — `emit_dispatch` is a no-op returning
0).
