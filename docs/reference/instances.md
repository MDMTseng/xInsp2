# Instances

How xInsp2 represents and manages plugin instances — read this if you touch
`cmd:create_instance` / `open_project` / `instance.json`, or you're tracing where
a piece of state lives.

## Plugin vs instance

- A **plugin** is a type — a DLL exposing the C ABI, registered once per backend
  start / rescan (`mock_camera`, `blob_analysis`).
- An **instance** is a configured use of a plugin in a project — a unique name,
  persisted JSON config, an on-disk folder, and a proxy the script calls via
  `xi::use("name")` (`cam0`, an instance of `mock_camera`).

A project may hold many instances of one plugin; each gets its own folder,
config, and identity.

## On-disk layout

```
<project>/
├── project.json        { name, script: "inspect.cpp", auto_respawn, watchdog_ms,
│                         parallelism, ... }
├── inspect.cpp
└── instances/
    └── cam0/
        ├── instance.json   { "plugin": "mock_camera", "config": { ... } }
        └── (whatever the plugin writes here)
```

`<project>/instances/<name>/` is per-instance scratch space — the plugin's
`host->instance_folder("cam0")` returns this path for calibration files, weights,
captured frames, anything bigger than the small `config` blob. Never auto-deleted.

## Lifecycle

| Command | What happens |
|---|---|
| `open_project` | Read `project.json` (respawn/watchdog/parallelism), scan `instances/`, for each: read `instance.json`, look up the plugin, build a `CAbiInstanceAdapter` via `xi_plugin_create(host, name)`, apply `config` via `set_def`, register. **Skip-bad-instance**: any failure records an `OpenWarning` and continues — the project opens with the survivors (read them via `cmd:open_project_warnings`). |
| `create_instance` | Validate name (unique, non-empty), create the folder, `xi_plugin_create(host, name)`, write initial `instance.json` from `get_def()`, register. |
| `save_project` | For each instance: `get_def()` → atomic write `instance.json` (`xi_atomic_io.hpp`, crash-safe). |
| `remove_instance` | Unregister (proxies start erroring), `xi_plugin_destroy`. **Folder kept by default** (recreate with the same name to resume); `--purge` to wipe. |
| `close_project` | `xi_plugin_destroy` all, clear registry, tear down the dispatch lanes. Plugin DLLs stay loaded. |
| Backend shutdown | `~PluginManager` `FreeLibrary`s every DLL explicitly (surfaces leaks under tools). |

## Registry

`xi::InstanceRegistry` (`xi_instance.hpp`) stores instances as
`shared_ptr<InstanceBase>`; the script proxy (`xi::use("...")`) holds a copy, so
it survives a hot-reload or close-project mid-call.

```cpp
class InstanceBase {
    virtual const std::string& name() const = 0;
    virtual const std::string& plugin_name() const = 0;
    virtual std::string        get_def() const;
    virtual bool               set_def(const std::string&);
    virtual std::string        exchange(const std::string& cmd);
};
```

`CAbiInstanceAdapter` (`xi_cabi_adapter.hpp`) is the one adapter — it holds the
`void*` instance + the resolved DLL function pointers. All instances run
in-process in the backend's address space.

## `instance.json` schema

Recognised top-level keys (anything else ignored, no error):

| Key | Type | Req | Notes |
|---|---|---|---|
| `plugin` | string | yes | The plugin this instance is bound to. |
| `config` | object | no | Passed verbatim to `set_def(json)` after construction. |
| `max_concurrency` | number | no | Per-instance concurrency cap under a parallel dispatch pool. Honoured only for a **reentrant** plugin (`plugin.json reentrant: true`); `0`/absent = unlimited; a non-reentrant plugin is always 1. |
| `group` | string | no | Dispatch group this source instance's triggers route to (a `project.json` `parallelism.groups[].name`). `""`/absent = `default_group`. Round-tripped by save. |
| `isolation` | string | no | **Deprecated, ignored** (any value) with a one-time warning — all instances run in-process (process isolation + SHM removed 2026-05; see [`../internals/fe-be.md`](../internals/fe-be.md)). |

`config` need **not** match `get_def()`'s shape — `set_def` ignores unrecognised
keys (so live telemetry in `get_def` like `frames_processed` has no place in the
seeded config). Treat `config` as "fields set at project-create time", `get_def`
as "everything the UI panel renders".

**Manifest validation** (on `open_project`): `config` keys are cross-checked
against the plugin's declared `manifest.params`; typos / type / range errors
surface in `cmd:open_project_warnings` (warnings-only, never blocks load — bad
keys default in `set_def`). Four kinds + details in
[`c-abi.md`](./c-abi.md) §6.

## Triggers

A source instance is an ordinary plugin that seals a pack and emits it
(`xi::PackOut f = new_pack(); … emit(std::move(f));` — the
`xi_pack_v1::emit_pack` door) from a worker thread; each emitted pack
dispatches one inspection, which the script reads from the trigger view
(`t.pack()`). There is no bus correlation — multi-camera capture is a
"gathering" plugin that emits one pack carrying N named image entries. Full
mechanics in [`../internals/dispatch.md`](../internals/dispatch.md).

## See also

- [`c-abi.md`](./c-abi.md) — what plugins export + consume.
- [`../guides/write-a-plugin.md`](../guides/write-a-plugin.md) — the task tour.
- `xi_plugin_manager.hpp` / `xi_cabi_adapter.hpp` / `xi_instance.hpp` — sources.
