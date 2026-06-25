# Instance lifecycle — a walkthrough

The end-to-end path of getting a plugin to *do work* in a project: make the
plugin discoverable → create an instance (and its on-disk folder) → call it from
the inspection script → delete it. This is the operator's tour; for the exact
contracts (`instance.json` schema, registry internals, load order) read
[`../reference/instances.md`](../reference/instances.md).

> **Plugin vs instance.** A **plugin** is a *type* — a DLL exposing the C ABI,
> registered once per backend scan (`mock_camera`, `blob_analysis`). An
> **instance** is a *configured use* of that plugin in one project — a unique
> name, persisted JSON config, an on-disk folder, and a proxy the script reaches
> via `xi::use("name")` (`cam0`, an instance of `mock_camera`). One project can
> hold many instances of one plugin; each gets its own folder and identity.

All the UI actions below live on the **Instances & Params** view — its title bar
has the high-frequency actions inline (`＋` Add Instance, Run/Stop, Pipeline
Graph) and the rest under the `⋯` overflow menu (Plugin Browser, New Project
Plugin, Rebuild Plugins, Project Settings…). There is no separate "Plugins"
view — the **Plugin Browser** (`⋯ → Plugin Browser`, opens as an editor tab) is
the single plugin-management surface.

---

## 1. Make the plugin discoverable

A plugin must be *scanned* before you can instance it. The backend scans a set
of root folders; any subfolder containing a `plugin.json` registers as a plugin
and shows up in `list_plugins`. Two ways to add a root:

| How | Stored in | Scope |
|---|---|---|
| **Plugin Browser → `＋ Add folder…`** (or **Add** on a browsed folder) | `project.json` → `plugin_dirs` (relative paths preferred — project stays portable) | this project |
| Command palette → **xInsp2: Add Plugin Folder…** | workspace setting `xinsp2.extraPluginDirs` (absolute paths) | across projects |

Two things to know:

- **Scanned ≠ built.** A source / `build: cmake` plugin under `plugin_dirs` only
  becomes a loadable DLL once you tick **compile** on it in the Browser (or hit
  **Rebuild** for a `build: cmake` plugin). Prebuilt DLLs and
  `<project>/plugins/` source plugins load on project open.
- After adding a folder the backend runs `rescan_plugins` → `list_plugins`, and
  the Browser's "added" list shows each plugin with a **●** loaded dot and its
  **×N used** count.

At this point the plugin *type* exists, but **no instance does yet**.

## 2. Create an instance (and its folder)

UI: **Instances & Params → `＋` (Add Instance)** → pick the plugin type → name it
(defaults to `<plugin>0`, e.g. `cam0`). The `＋` button only lights up once at
least one plugin is scanned (`xinsp2.hasPlugins`).

That fires `create_instance { name, plugin }`, and the backend
([`reference/instances.md`](../reference/instances.md) lifecycle table):

```
create_instance { name: "cam0", plugin: "mock_camera" }
   │
   ├─ validate name (non-empty, unique in the project)
   ├─ create folder   <project>/instances/cam0/
   ├─ xi_plugin_create(host, "cam0")          ← constructs the instance
   ├─ write instance.json  ← seeded from get_def()
   └─ register in xi::InstanceRegistry
```

On disk:

```
<project>/instances/cam0/
├── instance.json   { "plugin": "mock_camera", "config": { ... } }
└── (whatever the plugin writes — calibration, weights, captures)
```

`<project>/instances/cam0/` is that instance's private scratch space; the plugin
gets its path from `host->instance_folder("cam0")`. It is **never auto-deleted**.

## 3. Use it from the script

The instance **name** is the script's handle. In `inspect.cpp`:

```cpp
auto& det = xi::use("cam0");           // proxy → the registered instance
auto out  = det.process(input);        // forwards across the plugin ABI
```

- `xi::use("cam0")` looks the name up in `InstanceRegistry` and holds a
  `shared_ptr` copy, so the proxy survives a hot-reload or close-project
  mid-call (it starts erroring only after the instance is removed).
- A **source** instance (a camera) doesn't get called by the script — it calls
  `host->emit_record(...)` from its own worker thread; each record dispatches one
  inspection, which the script reads via `xi::current_trigger()`.
- After editing the script, **Compile** (`Ctrl+Shift+B`) recompiles + hot-loads.
  A `xi::use("name")` whose instance doesn't exist compiles fine but errors at
  run time — names are the only glue, so a typo shows up only when it runs.

## 4. Tune and persist

- Open an instance's panel with the **⚙ (Open Instance UI)** inline icon. Edits
  call `set_def(json)` live — no recompile.
- **Save Project** (or `save_project`) walks every instance: `get_def()` →
  atomic write back to its `instance.json` (crash-safe via `xi_atomic_io.hpp`).
- Next **Open Project** scans `instances/`, and for each reads `instance.json`,
  looks up the plugin, builds the adapter via `xi_plugin_create`, and applies
  `config` through `set_def`. A bad instance records an `OpenWarning` and is
  skipped — the project opens with the survivors (read them via
  `cmd:open_project_warnings`).

## 5. Delete an instance

UI: the **🗑 (Remove Instance)** inline icon on the instance row → a modal with
two choices:

| Choice | `remove_instance` arg | Effect |
|---|---|---|
| **Remove (keep folder)** | `delete_folder: false` | Unregister (proxies start erroring) + `xi_plugin_destroy`. `instances/<name>/` is **kept** — recreate an instance with the **same name** to resume from its calibration/weights. *(default)* |
| **Remove (and delete folder)** | `delete_folder: true` | Same, then wipe `instances/<name>/`. Not recoverable. |

The tree refreshes via `list_instances`. If the script still references a deleted
name, that `xi::use("…")` proxy errors at run time — remove the matching script
line too.

---

## Where each piece of state lives

| Thing | Lives in |
|---|---|
| Which folders are scanned for plugins | `project.json` `plugin_dirs` + `xinsp2.extraPluginDirs` setting |
| Which plugins exist (types) | backend scan → `list_plugins` (not persisted; rescanned each start) |
| Which instances exist + their config | `<project>/instances/<name>/instance.json` |
| An instance's bigger artifacts | `<project>/instances/<name>/` (via `host->instance_folder`) |
| The live, in-memory instance | `xi::InstanceRegistry` (gone on `close_project`) |

## See also

- [`../reference/instances.md`](../reference/instances.md) — schema, registry,
  load order, manifest validation, the lifecycle table.
- [`write-a-script.md`](./write-a-script.md) — `xi::use`, `current_trigger`, the
  script surface.
- [`write-a-plugin.md`](./write-a-plugin.md) — authoring the plugin behind an
  instance, `instance_folder`, `get_def`/`set_def`.
- [`../reference/ws-protocol.md`](../reference/ws-protocol.md) — `create_instance`
  / `remove_instance` / `open_project` / `rescan_plugins` wire shapes.
