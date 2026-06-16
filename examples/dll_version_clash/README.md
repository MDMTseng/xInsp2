# dll_version_clash — two plugins, same-named dependency, different versions

An experiment that answers: *"Plugin A needs `dep.dll` v1, plugin B needs `dep.dll`
v2 — what actually happens?"* All plugins load into **one** backend process, so
this is about the Windows loader's rules for same-named DLLs in a process.

Two identical probe plugins (`depprobe_a`, `depprobe_b`) each load a dependency
from their own folder and report **which version they actually got** and **the
path the module really loaded from**. The driver ships a different version of the
dependency into each plugin's folder (v1 → A, v2 → B) and tries three ways.

## Run

```sh
python examples/dll_version_clash/run_experiment.py
```

(Windows; needs the backend built. The driver asks the backend for the MSVC
location via `toolchain_health`, builds the two dependency versions with `cl`,
then drives the plugins over WebSocket.)

## What it shows

```
Phase 1 — full-path load, same base name 'dep1.dll'   -> A=1, B=2  NO collision
Phase 2 — by-name load,   same base name 'dep2.dll'   -> A=1, B=1  COLLISION
Phase 3 — by-name load,   distinct dep3a/dep3b.dll     -> A=1, B=2  NO collision
```

## The takeaway

The Windows loader keeps **one module per base name per process** — but that rule
only bites when a dependency is resolved **by base name**, which is exactly how a
**static import** (`#pragma comment(lib, ...)` / linking `foo.lib`) is resolved:

- **Phase 2 (the clash).** Both plugins resolve `dep2.dll` by name. probeA loads
  first; when probeB's load resolves the same base name, the loader returns the
  **already-resident** module — probeB silently runs v1 from *probeA's* folder.
  This is the real DLL-hell case for plugins that statically link a dependency.

- **Phase 3 (the usual fix).** Give the two dependencies **distinct file names**
  (and link each plugin against its own name). Different base names are independent
  modules, so both versions coexist.

- **Phase 1 (the escape hatch).** If a plugin loads its dependency **explicitly by
  absolute path** (`LoadLibraryEx(full_path, ...)`), the modern loader keys on the
  full path, so even same-named different-version DLLs from different folders stay
  distinct. Useful if you can't rename and control the loading yourself.

True same-name side-by-side versioning of a *static* import needs process
isolation, which xInsp2 doesn't provide (plugins run in-process for speed). See
[`docs/guides/write-a-plugin.md`](../../docs/guides/write-a-plugin.md) →
"Can I ship extra dependency DLLs with my plugin?".

## Files

| File | Purpose |
|---|---|
| `plugins/depprobe_{a,b}/plugin.cpp` | Identical probe: loads a dep by name or full path, reports version + loaded path |
| `dep_src.c` | The shared dependency; built twice with `/D VER=1` and `/D VER=2` |
| `run_experiment.py` | Builds the dep versions, drives the three phases, asserts the outcomes |
