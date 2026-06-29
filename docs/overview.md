# Overview — mental model + architecture in one picture

The mental model and the core nouns. Read once (~20 min); then the [index](./README.md)
maps tasks → docs.

## What xInsp2 is

A **Windows-first machine-vision inspection framework**. You build an inspection
*line* by composing **plugins** (cameras, detectors, ops, I/O) and a small **C++
script** that orchestrates them per frame. The bets:

- **Super-lightweight core, power via plugin composition.** The core does
  dispatch, lifecycle, crash-safety, and I/O plumbing; everything domain-specific
  is a plugin.
- **C++ for raw speed.** Scripts + plugins compile to DLLs; images *and* JSON move
  by pointer (zero-copy) through refcounted pools.
- **HDevelop-like iteration despite C++.** Fast edit→run via hot-reload + cached
  replay + hot params — not just fast compile.
- **Production-grade resilience.** A supervisor (FE) keeps the compute core (BE)
  respawning and records crash history; line safety is a comms plugin's own
  crash-watching sidecar process.

## The 60-second mental model

| Concept | What it is |
|---|---|
| **Project** | A folder: `project.json` + a `script` + `instances/` + `plugins/`. The unit you open, run, export. |
| **Script** (`inspect.cpp`) | The per-frame orchestration, compiled to a DLL. Calls plugins, emits results. One per project. |
| **Plugin** | A DLL implementing the C ABI: a camera/source, detector, op, or I/O bridge. Reusable across projects. |
| **Instance** | A *configured* plugin (`instances/<name>/instance.json`): which plugin + its config + its dispatch group. |
| **Record** | The data passed between script and plugin: schema-less JSON + a named-image bag. Zero-copy across the ABI. |
| **Trigger bus** | Turns each emitted record into one inspection run; a record carrying several named images (a gathering source) is inspected together. |
| **Dispatch group** | Worker threads (own `max_parallel` + OS priority + queue) that run inspections; groups are isolated. |
| **Run-result** | The one verdict per run: a signed status code + message (`>0` ok, `0` NA, `-1…` ng, `≤-990000` framework system-fail). Feeds HMI + PLC. |
| **HMI** | A standalone browser SPA that connects over WebSocket and *only* visualizes. |

## The architecture in one picture

```
              ┌───────────────────── one machine ─────────────────────┐
              │ xinsp-fe.exe ──spawns──► xinsp-backend.exe ─WS(:7823)─► HMI
              │ (supervisor, ROOT)       (compute core: trigger bus +   (browser
              │  respawn + crash history  dispatch groups + script +     viewer)
              │                           plugins — all in-process)
              └────────────────────────────────────────────────────────┘
 Dev box: the VS Code extension plays the FE's role (spawn/attach the BE) plus the
 edit→compile→run loop, instance/plugin trees, and webviews.
```

- **`xinsp-backend.exe` (BE)** — the compute core. Opens a project, compiles the
  script + plugins, runs continuous dispatch, serves a WebSocket API. All plugins
  run **in-process** (a hard plugin crash takes the BE down — the accepted trade
  for zero-copy speed; the FE respawns it).
- **`xinsp-fe.exe` (FE)** — the supervisor / root in production: spawn + monitor
  the BE, rate-limited respawn, record crash history, expose `fe_status`. PLC I/O
  is a plugin; line safety on a BE crash is that comms plugin's own sidecar
  process, not the FE. See [`internals/fe-be.md`](./internals/fe-be.md).
- **VS Code extension** — the dev front-end (spawn/attach BE, edit→run, trees,
  viewer). **HMI** (`hmi/`) — the production operator panel, a WS-only SPA
  ([`roadmap/production-hmi.md`](./roadmap/production-hmi.md)).

## The life of one inspection

1. A **source** plugin `emit_record`s a record (one or more named images +
   metadata) into its dispatch group.
2. The **trigger bus** turns that record into ONE event carrying its images; a
   *gathering* source that wants several frames inspected together puts them all in
   the same record.
3. The event runs in its **group's lane** on a worker thread (up to `max_parallel`
   parallel).
4. The **script** `xi_inspect_entry()` runs: reads the record's images, calls
   plugins (`xi::use("det").process(...)` → a `Record`), computes a verdict, emits
   `xi::result(code, msg)` (the one verdict), and may forward to a PLC via a comm
   plugin. (`VAR(...)` still compiles but no longer publishes per-run values — that
   path was removed from core; surfacing per-run values/images for viewing is now
   the shipped `preview` plugin's job — `PVAR` + `xi::preview::Sink`, see
   [`guides/write-a-script.md`](guides/write-a-script.md).)
5. Emission is ordered per group (so the stream stays in frame order under parallel
   compute).
6. The **HMI** renders the verdict; the **PLC** gets the verdict. Same
   run-result, multiple consumers.

How a trigger becomes a run in detail: [`internals/dispatch.md`](./internals/dispatch.md).
How Record crosses zero-copy: [`internals/data-layer.md`](./internals/data-layer.md).

## Codebase layout

```
backend/src/      service_main.cpp (BE — the big one), fe_main.cpp (supervisor),
                  runner_main.cpp (headless runner)
backend/include/xi/  the SDK headers (50+): xi_plugin_manager, xi_trigger_bus,
                  xi_image_pool, xi_doc_*, xi_use, xi_result, xi_types, …
backend/tests/    C++ unit tests (ctest) + benchmarks
hmi/              the operator SPA       vscode-extension/  the dev front-end
examples/         runnable projects + qa_*/driver.py regression suite
plugins/          globally-discoverable plugins
tools/            export_bundle.py, run_qa.py, xinsp2_py/ (Python SDK)
```

The single most important file is **`backend/src/service_main.cpp`** — command
handling, dispatch pools, script lifecycle, crash filter.

## Conventions worth knowing on day one

- **Windows-first, but keep new code cross-platform:** gate Win32 with `#ifdef
  _WIN32` + `// TODO(linux):` and record it in
  [`roadmap/linux-port.md`](./roadmap/linux-port.md).
- **Docs ride with code:** a behavior change updates its matching doc in the same
  commit.
- **`VAR(name, expr)` declares a local; reusing a name is a redefinition error**
  (`VAR(x, x)` → C2374). Common first-day gotcha. (Note: `VAR`/`EMIT` compile but
  no longer output anything — per-run output goes through the `preview` plugin.)

## Glossary

- **BE / FE** — backend (compute) / frontend (supervisor).
- **record / emit** — a source `emit_record`s a record (named images + metadata);
  each emit becomes one inspection run.
- **gathering source** — one plugin that emits several frames in a single record
  so they're inspected together (replaces multi-source correlation).
- **dispatch group** — a per-group worker lane (`max_parallel`, `thread_priority`, …).
- **run-result** — the one signed verdict code + message per run; the `≤-990000`
  band is framework-only (e.g. `XI_SYS_DROPPED`). See [`roadmap/run-result.md`](./roadmap/run-result.md).
- **AOT bundle** — a self-contained export the target runs with no toolchain.
- **working copy** — a transactional `.xinsp_work` scratch so edits survive a
  crash-respawn.
- **ImagePool / DocRegistry** — host-side refcounted pools; images and docs move
  by handle/pointer, zero-copy.

---

Where next: [`guides/build-and-run.md`](./guides/build-and-run.md) to build + run,
[`../CONTRIBUTING.md`](../CONTRIBUTING.md) if you're going to develop xInsp2 itself
(setup + test sweep + conventions), the [index](./README.md) for everything else.
