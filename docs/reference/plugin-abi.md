# Plugin C ABI

The exact contract a plugin DLL implements. Defined in
`backend/include/xi/xi_abi.h` (C side) and the `XI_PLUGIN_IMPL` macro
in `xi_abi.hpp` (C++ helper). This is the surface the host calls;
write a class with the right members and let the macro generate the C
exports.

---

## What the host expects

Every plugin DLL must export six C functions:

```c
extern "C" __declspec(dllexport)
void* xi_plugin_create(const xi_host_api* host, const char* name);

extern "C" __declspec(dllexport)
void  xi_plugin_destroy(void* inst);

extern "C" __declspec(dllexport)
void  xi_plugin_process(void* inst,
                        const xi_record* input,
                        xi_record_out*    output);

extern "C" __declspec(dllexport)
int   xi_plugin_exchange(void* inst,
                         const char* cmd,
                         char* rsp_buf, int rsp_buflen);

extern "C" __declspec(dllexport)
int   xi_plugin_get_def(void* inst, char* buf, int buflen);

extern "C" __declspec(dllexport)
int   xi_plugin_set_def(void* inst, const char* json);
```

All six are mandatory. `XI_PLUGIN_IMPL(YourClass)` generates them all
from a class with the right members — no need to write them by hand.

---

## Writing a plugin class

```cpp
class MyPlugin {
public:
    MyPlugin(const xi_host_api* host, const char* name)
        : host_(host), name_(name ? name : "") {}

    const xi_host_api* host() const { return host_; }     // required by XI_PLUGIN_IMPL

    std::string get_def() const                  { return "{}"; }
    bool        set_def(const std::string& json) { return true; }

    xi::Record process(const xi::Record& in) {
        return xi::Record{};
    }

    std::string exchange(const std::string& cmd) { return "{}"; }
};

XI_PLUGIN_IMPL(MyPlugin)
```

The macro expects:
- `Class(const xi_host_api*, const char*)` constructor.
- `host()` accessor returning `const xi_host_api*` (used by the macro
  to bridge `xi_record` ↔ `xi::Record`).
- `process(const xi::Record&) → xi::Record`.
- `exchange(const std::string&) → std::string`.
- `get_def() → std::string` (const).
- `set_def(const std::string&) → bool`.

Image sources additionally provide `grab(int timeout_ms) →
xi::Image`; see `xi/xi_source.hpp` and the `mock_camera` plugin.

---

## Lifecycle in detail

```
host scans plugin.json
        │
        ▼
host LoadLibrary(<name>.dll), resolves the 6 exports
        │
        ▼
xi_plugin_create(host_api, instance_name)        ── once per instance
        │
        ▼
xi_plugin_set_def(saved_config_json)             ── if persisted from before
        │
┌──── per inspection cycle ───────────────────────────────────────┐
│ xi_plugin_process(inst, input_record, output_record)            │
│ xi_plugin_exchange(inst, ui_command, rsp_buf, rsp_len)          │
└─────────────────────────────────────────────────────────────────┘
        │
        ▼ (on project save)
xi_plugin_get_def(buf, len) → JSON written to instance.json
        │
        ▼ (on host shutdown / project close)
xi_plugin_destroy(inst)
```

---

## Function-by-function contract

### `xi_plugin_create`

Called once when the host instantiates an instance.
- `host` — the function table; cache it.
- `name` — instance name (e.g. `"cam0"`); useful for logs and naming
  sub-resources.

Return your opaque instance pointer (the `new MyPlugin(...)`
result); the macro generates `try { ... } catch (...) { return
nullptr; }` so a throwing constructor cleanly fails the load.

Returning `nullptr` makes the host log a "factory returned null"
warning and skip the instance. The project as a whole still opens.

### `xi_plugin_destroy`

Mirror of create. The host calls this on:
- `cmd:remove_instance`
- project close
- host shutdown
- script reload of the *script* (not the plugin) — actually no, the
  plugin survives script reload.

Free anything you allocated in `xi_plugin_create`. The macro's default
just `delete`s the instance.

### `xi_plugin_process`

The hot path. Called once per inspection cycle (or once per trigger,
depending on bus policy).

```c
void xi_plugin_process(void*            inst,
                       const xi_record* input,
                       xi_record_out*   output);
```

- `input->images` — array of `{key, handle}`. Read pixels via
  `host->image_data(handle)`.
- `input->json` — null-terminated JSON object string. Use `xi::Json`
  or your favourite parser.
- `output` — fill via `xi_record_out_add_image()` (handle ownership
  transfers to the host) and `xi_record_out_set_json()` (string is
  strdup'd by the host).

Don't `image_release` input handles — the host owns them for the
call's duration. Don't `image_release` output handles — they belong
to the host after this returns.

#### Zero-copy across the ABI

Image bytes never get memcpy'd just because they cross the plugin
boundary. The C++ wrapper's `record_from_c` adopts each input handle
as a refcounted view (`xi::Image::adopt_pool_handle`) — the resulting
`xi::Image::data()` points directly at pool memory, not at a heap
copy. On the way out, `record_to_c` checks whether the returned
`xi::Image` is itself pool-backed; if it is (and from this same
host), it forwards the existing handle with an `addref` instead of
allocating a fresh slot and memcpy'ing pixels into it.

That makes pure forwarding (e.g. plugin A passing its input mask to
plugin B unchanged) genuinely zero-copy. Plugins that **produce new
pixels** (the usual case — gaussian, threshold, erode, …) still pay
exactly one memcpy when their freshly-allocated `xi::Image` lands in
the pool on the way out. That copy is structurally unavoidable until
operators are rewritten to write directly into pool slots via
`host->image_create()`; it's a future-work item, not a bug.

Cross-process isolation goes through the worker's SHM region rather
than the in-process pool, but the same handle-based contract applies
— the worker's RPC layer transfers handles, not bytes. See
`xi_process_instance.hpp::process_via_rpc`.

### `xi_plugin_exchange`

Generic RPC channel. Used for:
- UI button clicks (the UI panel posts JSON commands).
- Script-side calls via `xi::use<T>("name").exchange("...")`.

```c
int xi_plugin_exchange(void* inst, const char* cmd,
                       char* rsp_buf, int rsp_buflen);
```

Convention: `cmd` is a JSON object string, return a JSON object
string. `xi::Json` makes both directions a few lines.

Return value:
- `> 0` — number of bytes written to `rsp_buf` (excluding NUL).
- `< 0` — buffer too small; absolute value is bytes needed. Host
  retries with a bigger buffer.

### `xi_plugin_get_def` / `xi_plugin_set_def`

Persisted config. `get_def` is called on project save; `set_def` on
project load (or after `cmd:set_instance_def` from the UI).

Same `<n / -needed>` return contract as `exchange`.

Keep the JSON small (a few hundred bytes). For larger data, use
`host->instance_folder()` to get a directory and write whatever you
want.

---

## `XI_PLUGIN_IMPL` — what it actually generates

```cpp
extern "C" __declspec(dllexport)
void* xi_plugin_create(const xi_host_api* host, const char* name) {
    try { return new YourClass(host, name); }
    catch (...) { return nullptr; }
}

extern "C" __declspec(dllexport)
void xi_plugin_destroy(void* inst) {
    delete static_cast<YourClass*>(inst);
}

extern "C" __declspec(dllexport)
void xi_plugin_process(void* inst,
                       const xi_record* input,
                       xi_record_out* output) {
    auto* self = static_cast<YourClass*>(inst);
    xi::Record in_rec  = xi::record_from_c(self->host(), input);
    xi::Record out_rec = self->process(in_rec);
    xi::record_to_c(self->host(), out_rec, output);
}

/* ... exchange / get_def / set_def thunks the same way ... */
```

All six exports + `extern "C"` linkage. The macro lives at the bottom
of your `.cpp`:

```cpp
XI_PLUGIN_IMPL(MyPlugin)
```

---

## Plugin manifest

Every plugin folder needs a `plugin.json`:

```json
{
  "name":        "my_plugin",
  "description": "What this plugin does",
  "dll":         "my_plugin.dll",
  "factory":     "xi_plugin_create",
  "has_ui":      false
}
```

Fields:
- `name` (string, required) — the name the host registers; what
  scripts pass to `xi::use<>("...")` resolves the plugin against.
- `description` (string, optional) — shown in the plugins tree.
- `dll` (string, required) — DLL filename, relative to the plugin
  folder. `<name>.dll` is the convention.
- `factory` (string, optional) — exported create symbol; defaults to
  `xi_plugin_create`. Customising is rarely needed.
- `has_ui` (bool, optional) — if `true`, a `ui/index.html` next to the
  manifest is served as the plugin webview when the user opens the
  instance UI.
- `abi_version` (int, optional but written by `cmd:export_project_plugin`)
  — the `XI_ABI_VERSION` the plugin was compiled against. Matches the
  plugin DLL's `xi_plugin_abi_version()` export. Backends refuse
  plugins requesting a newer ABI than the host provides;
  pre-versioning plugins (no export) are accepted as v1 with a
  warning. Bump when `xi_abi.h` gains a load-bearing field plugins
  depend on.
- `manifest` (object, optional) — machine-readable description of the
  plugin's tunables and IO surface. The backend passes this through
  verbatim in `cmd:list_plugins` and `cmd:open_project` replies; it
  doesn't validate or reshape the contents. Convention:

  ```json
  "manifest": {
    "params":   [{"name": "...", "type": "int|float|bool|string",
                  "min": 0, "max": 10, "default": 0,
                  "enum": ["..."], "doc": "..."}],
    "inputs":   [{"name": "...", "kind": "image|json", "doc": "..."}],
    "outputs":  [{"name": "...", "kind": "image|json|number|bool|string",
                  "doc": "..."}],
    "exchange": [{"command": "...", "args": "...", "result": "...", "doc": "..."}]
  }
  ```

  Plugins without a manifest still work — listings just omit the
  field. The intent is letting AI agents and doc tools discover what a
  plugin does without grepping its source.

---

## Old C++ ABI (legacy)

A handful of older plugins use the **C++ ABI**:

```cpp
extern "C" __declspec(dllexport)
xi::InstanceBase* xi_plugin_create(const char* instance_name);
```

The host detects this by the absence of `xi_plugin_destroy` and falls
back to a `xi::InstanceBase` pointer + `delete`. New plugins should
use the C ABI / `XI_PLUGIN_IMPL` path; the C++ ABI is retained for
back-compat only.

---

## See also

- [`reference/host_api.md`](./host_api.md) — what the host gives the
  plugin.
- [`reference/instance-model.md`](./instance-model.md) — instance
  registry, persistence, isolation.
- [`guides/adding-a-plugin.md`](../guides/adding-a-plugin.md) — the
  task-shaped tour through writing a plugin.
- `backend/include/xi/xi_abi.h` — the canonical C ABI header.
- `backend/include/xi/xi_abi.hpp` — `XI_PLUGIN_IMPL` macro source.
