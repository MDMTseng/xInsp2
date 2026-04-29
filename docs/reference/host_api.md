# `xi_host_api` — host services exposed to plugins

Defined in `backend/include/xi/xi_abi.h`. The backend hands every
plugin a `const xi_host_api*` at construction; the plugin uses it to
allocate images, log, and (for sources) emit triggers. Pure C ABI, so
this is the contract that stays stable across compilers / versions.

```c
typedef struct xi_host_api {
    /* Image pool */
    xi_image_handle (*image_create)(int32_t w, int32_t h, int32_t channels);
    void            (*image_addref)(xi_image_handle h);
    void            (*image_release)(xi_image_handle h);
    uint8_t*        (*image_data)(xi_image_handle h);
    int32_t         (*image_width)(xi_image_handle h);
    int32_t         (*image_height)(xi_image_handle h);
    int32_t         (*image_channels)(xi_image_handle h);
    int32_t         (*image_stride)(xi_image_handle h);

    /* Logging */
    void (*log)(int32_t level, const char* msg);

    /* Per-instance scratch folder lookup */
    int32_t (*instance_folder)(const char* instance_name,
                               char* buf, int32_t buflen);

    /* Trigger bus emit (for source plugins) */
    void (*emit_trigger)(const char* source_name,
                         xi_trigger_id tid,
                         int64_t timestamp_us,
                         const xi_record_image* images,
                         int32_t image_count);
} xi_host_api;
```

The struct is **append-only**. Older plugin DLLs that don't read the
new tail fields stay binary-compatible.

> The `shm-process-isolation` spike branch appends `shm_create_image` /
> `shm_alloc_buffer` / `shm_addref` / `shm_release` /
> `shm_is_shm_handle` for opt-in cross-process zero-copy. Documented in
> `reference/ipc-shm.md` on that branch.

---

## Image pool

### Concept

Images are **refcounted handles**. Plugins never `malloc` / `free`
pixel buffers — they ask the host to allocate, get a `xi_image_handle`
(opaque `uint64_t`), read/write via `image_data`, and `image_release`
when done. Sharing between plugins is zero-copy: pass the handle.

### Allocate

```c
xi_image_handle h = host->image_create(640, 480, 1);
if (h == XI_IMAGE_NULL) return;          // out of memory
uint8_t* px = host->image_data(h);        // contiguous, stride = w*ch
```

Refcount starts at 1. `image_release` drops the ref; the buffer is
freed when refcount hits 0.

### Refcount semantics

- `image_addref(h)` increments the count. Use when caching an image
  you received as input.
- `image_release(h)` decrements. Free when zero.
- Invalid handles (already freed, never allocated) are no-ops.

### Receiving an input image

The host owns input handles inside `xi_record::images`. They're valid
for the duration of the `process()` call. To keep one across calls,
`image_addref` it; you're then responsible for the matching release.

### Returning an output image

Allocate via `host->image_create`, fill bytes, attach to
`xi_record_out` via `xi_record_out_add_image`. The host takes
ownership of output handles — **do NOT release them** before
`process()` returns.

### Stride / channels

`image_stride(h)` returns `width * channels` for the host's
contiguous-pixels layout. No padding. Plugins can rely on this for
SIMD-friendly access.

---

## Logging

```c
host->log(2, "warning message");   // 0=debug, 1=info, 2=warn, 3=error
```

Routed to backend stderr in the format `[<LEVEL>] <message>` and
visible in the VS Code extension's Output channel. Cheap; safe to call
from any thread.

---

## `instance_folder` — per-instance scratch space

```c
char buf[260];
int32_t n = host->instance_folder(instance_name, buf, sizeof(buf));
if (n > 0) {
    /* buf is a path without trailing slash, e.g.
       "C:\proj\my_project\instances\cam0" */
}
```

Properties:
- **Created before `xi_plugin_create()` runs**, so it's safe to write
  from inside your constructor.
- **Per instance, not per plugin.** Two instances of the same plugin
  each get their own folder.
- **Lives inside the project folder.** Copying / zipping the project
  carries instance data along.
- **Never deleted by the host.** Survives hot-reload, project
  open/close, host restart, instance recreate. Only the user can
  delete it.

Use for calibration images, ML weights, lookup tables, captured
reference frames — anything bigger than the small JSON config
returned by `get_def()`.

Returns 0 (and writes nothing) if the host doesn't recognise the
instance name (e.g. running headless without a project loaded).

---

## `emit_trigger` — for image-source plugins

Source plugins (cameras, simulators) push frames into the host's
TriggerBus by calling `emit_trigger` from a worker thread:

```c
xi_record_image rec_img = { "frame", h };
xi_trigger_id   tid     = { 0, 0 };       // host allocates if null
host->emit_trigger("cam0",
                   tid,                    // 128-bit; XI_TRIGGER_NULL OK
                   0,                      // ts_us; 0 = host clock
                   &rec_img, 1);
host->image_release(h);                   // bus addref'd internally
```

The bus correlates frames sharing a `tid` per the project's policy
(`Any` / `AllRequired` / `LeaderFollowers`) and dispatches one
inspection event per complete trigger.

`emit_trigger` is null on hosts older than the trigger bus addition;
plugins should null-check.

**Isolation gotcha**: when a source-plugin instance runs under the
current default `"isolation": "process"`, the worker gets a stub
`emit_trigger` that logs a warning and no-ops — emits can't reach
the backend's TriggerBus because the bus is a singleton in the
backend's address space, not the worker's. Source-plugin instances
must therefore set `"isolation": "in_process"` in their
`instance.json`. Cross-process emit_trigger is queued as future
work.

For threading safety inside source plugins, use `xi::spawn_worker`
(see `xi/xi_thread.hpp`) — it installs the SEH translator on the
worker thread so a segfault becomes a recoverable C++ exception.

---

## Idioms

### Mid-call image creation

```c
xi_image_handle dst = host->image_create(w, h, 1);
uint8_t* dp = host->image_data(dst);
/* fill dp[0..w*h] ... */
xi_record_out_add_image(out, "binary", dst);
/* DO NOT release dst — output ownership transfers to host */
```

### Caching an input image across calls

```c
xi_image_handle template_img = input->images[0].handle;
host->image_addref(template_img);             // we keep a ref
saved_template_ = template_img;                // caller owns input handles only for the call
/* ... in destroy: */
host->image_release(saved_template_);
```

### Cross-plugin handoff

The host already addrefs handles when they pass through `xi::Record`
between plugins via `xi::use(...).process(...)`. Plugins don't need to
manage refs in that case — the host's record bridge does it.

---

## See also

- [`reference/plugin-abi.md`](./plugin-abi.md) — the C exports a
  plugin DLL must provide.
- [`reference/instance-model.md`](./instance-model.md) — how instances
  are loaded, persisted, and torn down.
- `backend/include/xi/xi_abi.h` — authoritative source.
