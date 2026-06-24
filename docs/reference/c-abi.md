# C ABI — plugin exports + host services

The complete, stable C contract between the host and a plugin DLL. Two halves:
the **exports a plugin provides** and the **`xi_host_api` services the host hands
back**. Canonical source: `backend/include/xi/xi_abi.h` (C) + the
`XI_PLUGIN_IMPL` macro in `xi_abi.hpp` (C++ helper). Pure C, so it stays stable
across compilers and versions.

You normally write a C++ class and let `XI_PLUGIN_IMPL` generate the C exports —
see [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) for the task tour;
this page is the exact contract.

---

## 1. Plugin exports

Every plugin DLL exports six C functions (all mandatory):

```c
void* xi_plugin_create(const xi_host_api* host, const char* name);
void  xi_plugin_destroy(void* inst);
void  xi_plugin_process(void* inst, const xi_record* input, xi_record_out* output);
int   xi_plugin_exchange(void* inst, const char* cmd, char* rsp_buf, int rsp_buflen);
int   xi_plugin_get_def(void* inst, char* buf, int buflen);
int   xi_plugin_set_def(void* inst, const char* json);
```

`XI_PLUGIN_IMPL(YourClass)` generates all six (plus the `xi_plugin_abi_version`
and `xi_yyjson_abi` stamps — see §4) from a class with these members:

```cpp
class MyPlugin {
public:
    MyPlugin(const xi_host_api* host, const char* name);
    const xi_host_api* host() const;              // the macro bridges xi_record ↔ xi::Record
    xi::Record  process(const xi::Record& in);
    std::string exchange(const std::string& cmd);
    std::string get_def() const;
    bool        set_def(const std::string& json);
};
XI_PLUGIN_IMPL(MyPlugin)                           // at the bottom of the .cpp
```

Image sources are ordinary plugins too — they push frames by calling
`host->emit_record(...)` from a worker thread (see the `mock_camera` plugin);
there is no separate source interface or pull/`grab` model.

**Optional (ABI v7) — frame-perfect config swap.** A heavy-resource plugin (one
that loads big assets — model weights, templates, calibration — on a config
change) can opt into a two-phase swap so the load never stalls the pipeline:

```c
int  xi_plugin_prepare(void* inst, const char* def_json, const char* folder);  /* 0 = ok */
void xi_plugin_commit(void* inst);
```

Override `prepare()` / `commit()` on your class and add `XI_PLUGIN_STAGED(MyPlugin)`
*after* `XI_PLUGIN_IMPL`. `prepare()` loads the new config's assets into a
**background staging slot**; the host calls it **UNGATED** (concurrent with
`process()`), so it must touch **only** the staging slot — never live state.
`commit()` then **atomically swaps** staging → live; the host calls it **gated**
(serialized vs `process()`, and under `commit_group` after draining dispatch).
Canonical pattern: hold the live config in a `std::atomic<std::shared_ptr<const
T>>` that `process()` reads lock-free, `prepare()` builds a new one, `commit()`
swaps the pointer. A plugin that omits these falls back to gated `set_def`
(prepare) / no-op (commit), so simple plugins need neither. The orchestrator
drives them via `prepare_instance` + `commit_group` → `reference/ws-protocol.md`,
`roadmap/config-bundles-and-orchestration.md`, and `plugins/config_swap_probe/`.

### Lifecycle

```
scan plugin.json → LoadLibrary → resolve 6 exports → ABI/yyjson load gate (§4)
  → xi_plugin_create(host, name)              once per instance
  → xi_plugin_set_def(saved_config)           if persisted
  ── per cycle: xi_plugin_process / xi_plugin_exchange ──
  → xi_plugin_get_def(buf,len)                on project save
  → xi_plugin_destroy(inst)                   on remove / close / shutdown
```

The plugin **survives script reload** (only the script DLL reloads).

---

## 2. Function-by-function contract

**`xi_plugin_create(host, name)`** — once per instance. Cache `host`; `name` is
the instance name (`"cam0"`). Return your opaque instance pointer. The macro
wraps it in `try { … } catch (…) { return nullptr; }`, so a throwing constructor
fails the load cleanly (host logs "factory returned null", skips the instance,
the project still opens).

**`xi_plugin_destroy(inst)`** — mirror of create; free what create allocated.

**`xi_plugin_process(inst, input, output)`** — the hot path, once per cycle.
- `input->images` — `{key, handle}` array; read pixels via `host->image_data(h)`.
  Handles are **zero-copy views** over the pool, valid for the call's duration.
- `input->data`/`input->len` — JSON bytes, used **iff `input->doc == NULL`**.
  `input->doc` — a borrowed `yyjson_mut_doc*` for the in-process fast path
  (zero-serialize); the C++ wrapper hides this behind `xi::Record`.
- Fill `output` via `xi_record_out_add_image()` (handle ownership → host) and the
  JSON/doc path (the C++ wrapper's `record_to_c` handles it).
- **Don't release input handles** (host owns them for the call) **or output
  handles** (host owns them after return).

Zero-copy: forwarding an unchanged image (plugin A's input mask → plugin B) is
genuinely zero-copy — `record_from_c` adopts each handle as a refcounted view,
`record_to_c` re-forwards a pool-backed output handle with an `addref` instead of
a memcpy. A plugin that **produces new pixels** pays exactly one memcpy when its
fresh `xi::Image` lands in the pool on the way out (structurally unavoidable
until operators write straight into pool slots).

**`xi_plugin_exchange(inst, cmd, rsp, len)`** — generic JSON-in/JSON-out RPC, for
UI button clicks and `xi::use("name").exchange(...)`. Return `> 0` = bytes
written (excl. NUL); `< 0` = buffer too small, `|value|` = bytes needed (host
retries). Convention: return the post-command `get_def()` so a UI/script gets a
single mutate-and-observe round-trip. For the unknown-command fallthrough use
`xi::Plugin::exchange_unknown_command(name)` → `{"error":"unknown_command",
"command":"<name>"}` (a misspelled command otherwise looks like a successful
no-op).

**`xi_plugin_get_def` / `xi_plugin_set_def`** — persisted config (get on save,
set on load / `cmd:set_instance_def`). Same `<n / -needed>` return contract as
`exchange`. Keep the JSON small (hundreds of bytes); for bigger data use
`host->instance_folder()`.

---

## 3. The host API — `xi_host_api`

The host hands every plugin a `const xi_host_api*` at construction. Append-only
struct; null-check tail fields (an older host may leave them `nullptr`).

| Group | Fields | Notes |
|---|---|---|
| Image pool | `image_create` / `image_addref` / `image_release` / `image_data` / `image_width`/`height`/`channels`/`stride` | Refcounted opaque `uint64` handles; see below. |
| Logging | `log(level, msg)` | 0=debug … 3=error → backend stderr. Any thread. |
| Image I/O | `read_image_file(path)` | Decode an image file straight into the pool (v1). |
| Status | `set_status(source, text)` | Push a status line to the FE status channel → `reference/ws-protocol.md`. |
| Instance | `instance_folder(name, buf, len)` | Per-instance scratch dir, created before `create()`; never auto-deleted. |
| Dispatch (v6) | `emit_record(emitter, id, rec, ts)` | **The one dispatch verb.** A source hands the host a record (images + metadata doc); the host dispatches one inspection. The script reads it via `current_trigger().image()/.meta()/.id_string()`. Metadata rides by pointer (zero-serialize). Use the SDK helper `xi::emit_record`. → `internals/dispatch.md`. |
| Doc allocator (v3, γ) | `doc_chunk_alloc` / `doc_chunk_realloc` / `doc_chunk_free` | Host-owned pool behind the in-process yyjson doc → `internals/data-layer.md`. |
| Doc refcount (v4, γ-4) | `doc_retain` / `doc_release` / `doc_refcount` | The doc analogue of `image_addref/release` → `internals/data-layer.md`. |
| SHM (removed 2026-05) | `shm_*` (5) | Always `nullptr`; kept for binary compat. Fall back to `image_create`. |

### Image pool — the refcount contract

Images are **refcounted handles**, never raw `malloc`/`free`. `image_create`
returns a handle at refcount 1; `image_addref` increments, `image_release`
decrements (freed at 0; invalid handles are no-ops). `image_stride(h)` is
`width*channels` (no padding — SIMD-safe). Ownership rules:
- **Input handles** belong to the host for the `process()` call. To keep one
  across calls, `image_addref` it and `image_release` it in `destroy`.
- **Output handles** transfer to the host on `process()` return — don't release.
- Cross-plugin handoff through `xi::Record` is auto-refcounted by the host bridge.

(The full image table + wiring is generated by `make_host_api()` in
`xi_image_pool.hpp`.)

---

## 4. ABI version + load gate

`XI_ABI_VERSION` is **7**. The struct was append-only through v5; v6 broke that
to remove the retired dispatch fields, so **all plugins must be rebuilt against
the current ABI** (no binary compat with v4/v5 kept — this is pre-stable-release).
v7 added only OPTIONAL *plugin* exports (not `xi_host_api` fields), so it shifts no
struct — an older v6 plugin still loads on a v7 host. History:

| ver | added |
|---|---|
| 1 | image pool, trigger bus, SHM (since removed), `read_image_file` |
| 2 | emit/fetch resource hooks + `set_safe_state` (both removed in v6) |
| 3 | in-process doc allocator `doc_chunk_*` (γ) |
| 4 | in-process doc refcount `doc_retain`/`doc_release`/`doc_refcount` (γ-4) |
| 5 | `emit_trigger_record` — trigger metadata doc (superseded by v6) |
| 6 | dispatch collapsed to ONE verb `emit_record(emitter,id,rec,ts)`; `emit_trigger`, `emit_resource`, `fetch_resource`, `fetch_image`, `emit_dispatch` REMOVED (no v4 compat — rebuild all plugins). Multi-cam = gathering plugin; replay = buffer-replay plugin. |
| 7 | optional plugin exports `xi_plugin_prepare(inst,def,folder)` + `xi_plugin_commit(inst)` for frame-perfect config swap (opt in via `XI_PLUGIN_STAGED`; prepare ungated, commit gated). PLUGIN exports, not host-api fields — no struct shift, older plugins still load. |

**Two load gates** (a plugin failing either is refused at load with a clear
error, then `FreeLibrary`'d):
1. **ABI version** — `xi_plugin_abi_version()` asking for a version *newer* than
   the host is refused.
2. **yyjson layout** (γ-4) — `xi_yyjson_abi()` must match the host's stamp. A
   mismatch (different yyjson build) or no export means the plugin can only run
   the slow JSON-serialize path, so it is **refused unless** `plugin.json` sets
   `"json_fallback": true` (then it loads on the JSON path with a one-shot
   warning). Plugins built with `XI_PLUGIN_IMPL` against the host's vendored
   yyjson pass automatically. See `internals/data-layer.md`.

> **Migration note (v6).** "Rebuild all plugins" also covers your **native
> tests**: the plugin certification suite (`xi/xi_baseline.hpp` + `xi/xi_cert.hpp`
> — `xi::baseline::load_symbols` / `run_all`, `xi::cert::certify`) was removed
> with the cert gate. A test that used it must resolve the plugin's C-ABI exports
> directly: `LoadLibrary` the DLL, then `GetProcAddress` for `xi_plugin_create` /
> `xi_plugin_destroy` / `xi_plugin_process` (+ `get_def`/`set_def`/`exchange` as
> needed), and assert behaviour with `xi/xi_test.hpp` (which survives). See
> `sdk/examples/counter/tests/test_counter.cpp` for the pattern.

---

## 5. Record at the boundary

The in-process fast path passes the yyjson doc by pointer (zero-serialize);
`data`/`len` carry JSON bytes only when the doc pointer is null.

```c
typedef struct { const char* key; xi_image_handle handle; } xi_record_image;
typedef struct {
    const xi_record_image* images; int32_t image_count;
    const uint8_t* data; int32_t len;   /* JSON bytes — used iff doc == NULL */
    const void* doc;                    /* borrowed yyjson_mut_doc* (in-process) */
} xi_record;
typedef struct {
    xi_record_image* images; int32_t image_count; int32_t image_capacity;
    const uint8_t* data; int32_t len;   /* JSON bytes — used iff out_doc == NULL */
    void* out_doc;                      /* adopted yyjson_mut_doc* (zero-copy) */
} xi_record_out;
```

---

## 6. Plugin manifest — `plugin.json`

```json
{ "name": "my_plugin", "description": "...", "dll": "my_plugin.dll",
  "factory": "xi_plugin_create", "has_ui": false }
```

| Field | Meaning |
|---|---|
| `name` (req) | Registered name; what `xi::use("...")` resolves against. |
| `dll` (req) | DLL filename relative to the folder; `<name>.dll` by convention. |
| `description` / `factory` / `has_ui` | Tree label / create symbol (default `xi_plugin_create`) / serve `ui/index.html` as the webview. |
| `reentrant` (alias `thread_safe`) | `process`/`exchange`/`get_def`/`set_def` are safe to call concurrently on one instance. Default `false` = host serializes per instance (so a parallel dispatch pool is safe by default). Set `true` only if internally thread-safe. **Which type are you + the caveats** (T0 stateless-free, T1 host-protected, T2 lock-it-yourself, T3 double-slot): see [`guides/write-a-plugin.md`](../guides/write-a-plugin.md) → "Concurrency & config-change safety". |
| `json_fallback` | Allow load despite a yyjson-layout mismatch (runs the slow JSON path). Default `false` = refused; see §4 + `internals/data-layer.md`. |
| `build` (alias `prebuilt`) | `"source"` (default) = backend compiles the plugin's `.cpp` with `cl.exe`. `"cmake"` (or `"prebuilt": true`) = the plugin owns its build (own `CMakeLists.txt`, for external libs / CUDA); the backend loads the prebuilt `<name>.dll` and `cmd:rebuild_plugins` runs its CMake. See `guides/write-a-plugin.md` (External libraries & CUDA). |
| `abi_version` | The `XI_ABI_VERSION` compiled against (written by `cmd:export_project_plugin`); gated per §4. |
| `manifest` (object) | Machine-readable tunables + IO surface (`params`/`inputs`/`outputs`/`exchange`), passed through verbatim for tooling/agents. |

**`manifest.params` validation** (on `cmd:open_project`): each instance's
`config` is walked against the declared params; mismatches emit an
`open_project_warnings` entry (`unknown_config_key` / `type_mismatch` /
`out_of_range` / `not_in_enum`). Warnings-only — the bad value still flows to
`set_def` (which defaults it); a typo never blocks project load. Plugins without
`manifest.params` skip validation.

---

## Legacy C++ ABI

A few old plugins use `xi::InstanceBase* xi_plugin_create(const char*)` (no
`xi_plugin_destroy` export). The loader detects the C ABI by the **presence** of
`xi_plugin_destroy`; absence → the legacy C++ path. New plugins must use the C
ABI / `XI_PLUGIN_IMPL`; the C++ path is back-compat only.

## See also

- [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) — the task tour.
- [`data-types.md`](./data-types.md) — Record / Image / typed I/O at the boundary.
- [`instances.md`](./instances.md) — instance load / persist / teardown.
- `backend/include/xi/xi_abi.h` + `xi_abi.hpp` — canonical headers.
