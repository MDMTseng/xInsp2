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
    │   │                          "isolation": "process"?,
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
   - If `instance.json` has `"isolation": "process"` AND the worker env
     is configured, build a `ProcessInstanceAdapter` (plugin runs in
     `xinsp-worker.exe`, method calls go over IPC, pixel data via SHM).
     Falls back to in-proc with a warning if the worker env is missing.
   - Otherwise (default): in-proc — `xi_plugin_create(host_api, instance_name)`.
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

Two adapters wrap the C ABI:
- `CAbiInstanceAdapter` — same-process: holds the `void*` instance
  pointer and the function pointers resolved from the DLL.
- `ProcessInstanceAdapter` — cross-process (spike branch): proxies
  every call over a named pipe to a `xinsp-worker.exe` instance,
  with auto-respawn + per-call timeout.

---

## isolation modes

**Default: in-proc.** Plugin instance lives in the backend's address
space and is reached via direct C ABI calls. Fastest, but a plugin AV
or heap corruption can take the backend with it.

**Opt-in `"isolation": "process"`** — plugin runs in
`xinsp-worker.exe`, method calls (`set_def` / `exchange` / `get_def`)
proxy over a named pipe, pixel data shares zero-copy via SHM. A buggy
plugin can crash its worker process without taking the backend with it;
`ProcessInstanceAdapter` auto-respawns the worker (rate-limited 3/60s)
and replays the last `set_def` so the next call still works.

```json
{
  "plugin": "shape_match",
  "isolation": "process",
  "call_timeout_ms": 60000,
  "config": { ... }
}
```

`call_timeout_ms` (optional, isolated instances only) bounds how long
a single IPC call (`process` / `exchange` / `set_def` / `get_def`)
may block before the adapter cancels it via `CancelIoEx` and treats
the worker as crashed. Default 30 s — bump it for plugins with slow
operations (long-exposure cameras, heavy ML inference, big template
matches) so the watchdog doesn't trip during normal work.

Worker-side conveniences (so plugin authors don't need to know which
mode they're running in):

- Plugins that allocate output via `xi::Image{...}` (heap-pool) get
  their pixels auto-copied into SHM by `worker_main` before the reply.
  The plugin sees a normal heap allocation; the backend sees an SHM
  handle.
- The output image's key (`record.image("mask", img)` → `"mask"`) is
  preserved across the IPC boundary, so scripts calling
  `record.get_image("mask")` work the same in-proc and isolated.

**Default-on is tracked work** — see `status.md`. Unblocking it needs
broader real-plugin testing (multi-image Records, plugins that store
handles in their JSON, error paths, hot-reload semantics).

A spawned `xinsp-worker.exe`:
- Attaches the backend's SHM region (so image handles dereference to
  the same physical pages → zero-copy `process()`).
- Loads the plugin DLL on its side.
- Services RPC over a named pipe.
- A crash in the plugin only kills the worker; backend respawns it
  (rate-limited 3/60s).
- A hung `process()` is bounded by the per-call timeout
  (default 30s) → `CancelIoEx` watchdog → respawn.

The isolation choice is per-instance, not per-project — you can mix
isolated and in-proc instances in the same project. Useful for
sandboxing one suspect / third-party plugin without paying RPC
overhead on the rest.

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
