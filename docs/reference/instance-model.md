# Instance model

How xInsp2 represents and manages plugin instances. Read this if
you're building a feature that touches `cmd:create_instance` /
`open_project` / `instance.json`, or if you're trying to figure out
where a piece of state lives.

---

## Plugin vs instance

- A **plugin** is a type — a DLL exposing the C ABI, registered once
  per backend startup (or rescan). Examples: `mock_camera`,
  `blob_analysis`.
- An **instance** is a configured use of a plugin in a project — it
  has a unique name, persisted JSON config, an on-disk folder, and a
  proxy object the script can call via `xi::use("name")`. Example:
  `cam0` (an instance of `mock_camera`).

A project can have many instances of the same plugin; each gets its
own folder, config, and identity.

---

## On-disk layout

```
<project>/
├── project.json
│   {
│     "name": "...",
│     "script": "inspection.cpp",
│     "auto_respawn": true,
│     "watchdog_ms": 10000,
│     "trigger_policy": { ... }
│   }
├── inspection.cpp
└── instances/
    ├── cam0/
    │   ├── instance.json     ← { "plugin": "mock_camera",
    │   │                          "isolation": ...?  (deprecated, ignored),
    │   │                          "config": { ... } }
    │   └── (whatever the plugin chose to write here)
    └── det0/
        └── instance.json
```

`<project>/instances/<name>/` is **per-instance scratch space**. The
plugin's `host->instance_folder("cam0")` returns this path; the
plugin can write calibration files, ML weights, captured frames,
anything bigger than `instance.json`'s small config blob.

---

## Lifecycle

### `cmd:open_project`

1. Backend reads `project.json` → applies trigger policy, autorespawn,
   watchdog.
2. Backend scans `instances/` subdirectories.
3. For each subdirectory:
   - Read `instance.json`. Field `plugin` names the type.
   - Look up the plugin in the registered set (scanned earlier from
     `plugins_dir` + extra dirs).
   - Build the in-process adapter — `xi_plugin_create(host_api, instance_name)`.
     (A legacy `"isolation"` field, if present, is accepted but ignored
     with a one-time deprecation warning; everything runs in-process.)
   - Apply persisted `config` via `xi_plugin_set_def`.
   - Register in `InstanceRegistry`.
4. **Skip-bad-instance**: any failure (broken JSON, missing plugin,
   factory throws) records an `OpenWarning` and continues. The
   project still opens with the survivors.

The list of skipped instances is surfaced as a `warn` log; the user
can read it via `cmd:open_project_warnings` and decide whether to fix
or delete the bad folder.

### `cmd:create_instance`

Adds a new instance to a running project:
1. Validates name (uniqueness, not empty).
2. Creates `<project>/instances/<name>/`.
3. Calls `xi_plugin_create(host_api, name)` (this is when the plugin
   first sees its instance name).
4. Writes initial `instance.json` with the plugin's `get_def()`
   output.
5. Registers in `InstanceRegistry`.

### `cmd:save_project`

For every instance in the registry: call `get_def()`, write
`instances/<name>/instance.json`. Atomic write via
`xi_atomic_io.hpp` so a crash mid-save can't corrupt config.

### `cmd:remove_instance`

1. Remove from `InstanceRegistry` (script's `xi::use(...)` proxies
   start returning errors).
2. Call `xi_plugin_destroy`.
3. **Default: keep the folder on disk.** The user can recreate the
   instance with the same name and pick up where they left off. Pass
   the `--purge` flag (or click "Delete folder too" in the UI) to
   wipe.

### `cmd:close_project`

Tear down all instances (call `xi_plugin_destroy` on each), clear the
registry. Plugin DLLs stay loaded; only instance objects go.

The trigger bus is reset; recording is stopped.

### Backend shutdown

Dtors run in `~PluginManager`: `FreeLibrary` every loaded plugin DLL
explicitly. Cleaner than letting the OS reap them — surfaces leaks
under detection tools.

---

## Instance registry

`backend/include/xi/xi_instance.hpp` — the global
`xi::InstanceRegistry`.

```cpp
auto& reg = xi::InstanceRegistry::instance();
auto inst = reg.find("cam0");                // shared_ptr<InstanceBase>
auto all  = reg.list();                      // vector
reg.add(my_instance);
reg.remove("cam0");
```

Instances are stored as `shared_ptr<InstanceBase>`; the script
proxy (`xi::use<T>("...")`) takes a copy so it survives even if a
hot-reload or close-project happens mid-call.

`InstanceBase` is a simple polymorphic interface:

```cpp
class InstanceBase {
public:
    virtual ~InstanceBase() = default;
    virtual const std::string& name() const = 0;
    virtual std::string        plugin_name() const = 0;
    virtual std::string        get_def() const             { return "{}"; }
    virtual bool               set_def(const std::string&) { return true; }
    virtual std::string        exchange(const std::string& cmd) { return "{}"; }
};
```

One adapter wraps the C ABI:
- `CAbiInstanceAdapter` — in-process: holds the `void*` instance
  pointer and the function pointers resolved from the DLL. This is the
  only adapter path; all instances run in the backend's address space.

---

## isolation modes

**All instances run in-process.** The backend is a single in-process
compute core (BE) under a frontend (FE) supervisor; every plugin —
cameras included — is called directly through the in-process
`CAbiInstanceAdapter`, zero-copy via pointers. There is no worker
process, no IPC, and no shared-memory region.

The earlier process-isolation + SHM mesh was removed 2026-05. The
rationale: a dead plugin means a dead pipeline regardless of
isolation, so per-plugin sandboxing bought only complexity. Crash
diagnosability (minidumps + per-thread breadcrumbs + PDB
symbolication, see `guides/debugging.md`) is the replacement safety
net.

**The `"isolation"` field is deprecated.** Old `instance.json` files
that carry `"isolation"` (any of `"process"` / `"in_process"` /
`"none"`) still load — the field is accepted but ignored, emitting a
one-time deprecation warning. It is documented only so you know old
projects keep working; new projects should omit it.

```json
{
  "plugin": "shape_match",
  "config": { ... }
}
```

### `instance.json` schema

Recognised top-level keys (anything else is ignored, no error):

| Key | Type | Required | Notes |
|---|---|---|---|
| `plugin` | string | yes | Name of the plugin this instance is bound to. |
| `config` | object | no | Passed verbatim to `Plugin::set_def(json)` after construction. |
| `max_concurrency` | number | no | Per-instance concurrency cap under a parallel dispatch pool (`parallelism.dispatch_threads > 1`). Bounds how many workers may be inside this instance's entry points at once. Only honoured for a **reentrant** plugin (`plugin.json` `reentrant: true`); `0`/absent = unlimited. A non-reentrant plugin is always 1. See `docs/guides/writing-a-script.md` (Parallel dispatch). |
| `isolation` | string | no | **Deprecated, ignored.** Accepted for backward compatibility (any value) with a one-time deprecation warning; all instances run in-process. |

`config` is **not** required to be the same shape as `Plugin::get_def()`'s
return. Plugins commonly include read-only telemetry in `get_def()`
(`frames_processed`, `last_count`, etc.) that has no place in the
seeded `instance.json` config — `set_def()` just ignores keys it
doesn't recognise. Treat `config` as "fields the user may want to set
at project-create time"; treat `get_def()` as "everything the UI panel
needs to render the live state".

**Validation against the plugin manifest.** On `cmd:open_project` the
backend cross-checks each `config` key against the plugin's declared
`manifest.params` (if any) and surfaces typos / type errors / out-of-
range values as entries in `cmd:open_project_warnings`. Validation
never blocks load — bad keys still fall through to `set_def`, which
defaults silently. See `docs/reference/plugin-abi.md` "Plugin
manifest" for the four warning kinds (`unknown_config_key`,
`type_mismatch`, `out_of_range`, `not_in_enum`). Plugins without a
`manifest.params` block skip validation entirely.

---

## Trigger bus + instances

Sources publish frames via `host->emit_trigger`. Each instance of an
`xi::ImageSource` plugin gets a TriggerBridge that routes its emits
into the bus tagged by the instance's name (the `source_name`
parameter of `emit_trigger`).

The project's trigger policy decides when frames correlate:
- `Any` — every emit fires one inspect.
- `AllRequired` — wait for emits from every named source under the
  same `tid`, then fire one inspect.
- `LeaderFollowers` — wait for the leader; attach the most recent
  followers; fire on leader's emit.

Configured in `project.json: trigger_policy`. Instances don't know or
care about policy — they just emit; the bus does the rest.

---

## See also

- [`reference/host_api.md`](./host_api.md) — the C ABI plugins consume.
- [`reference/plugin-abi.md`](./plugin-abi.md) — what plugins must
  export.
- [`guides/adding-a-plugin.md`](../guides/adding-a-plugin.md) — the
  task-shaped tour.
- `backend/include/xi/xi_plugin_manager.hpp` — the canonical
  registry / loader implementation.
- `backend/include/xi/xi_instance.hpp` — `InstanceBase` /
  `InstanceRegistry`.
