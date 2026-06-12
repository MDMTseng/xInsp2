# Getting started — onboarding for a new team

**Read this first.** It gives you the mental model, the architecture in one
picture, and a guided path into the detailed docs. Budget ~30 minutes; you'll
build + run something by the end.

> Already oriented? The [doc index](./README.md) is task-shaped; jump straight to
> the verb that matches what you're doing.

---

## 1. What xInsp2 is (and the philosophy)

xInsp2 is a **Windows-first machine-vision inspection framework**. You build an
inspection *line* by composing **plugins** (cameras, detectors, ops, I/O) and a
small **C++ script** that orchestrates them per frame. The design bets:

- **Super-lightweight core, power via plugin composition.** The core does
  dispatch, lifecycle, crash-safety, and I/O plumbing. Everything domain-specific
  (a detector, a camera, a PLC bridge) is a plugin.
- **C++ for raw speed.** Scripts and plugins are C++ compiled to DLLs; images move
  by pointer (zero-copy) through a refcounted pool.
- **HDevelop-like iteration despite C++.** Fast edit→run via hot-reload + cached
  replay + hot params — not just fast compile.
- **Production-grade resilience.** A supervisor (FE) keeps the line safe and the
  compute core (BE) respawning; a PLC dead-man chain survives a crash.

The long-form vision is in [`status.md`](./status.md) (what ships now vs in
flight) and the design docs under [`design/`](./design/).

---

## 2. The 60-second mental model

Internalize these eight nouns and you can read any part of the system:

| Concept | What it is |
|---|---|
| **Project** | A folder: `project.json` + a `script` + `instances/` + `plugins/`. The unit you open, run, and export. |
| **Script** (`inspect.cpp`) | The per-frame orchestration logic. Compiled to a DLL. Calls plugins, emits results. One per project. |
| **Plugin** | A DLL implementing `xi::Plugin`: a camera/source, a detector, an op, an I/O bridge. Reusable across projects. |
| **Instance** | A *configured* plugin (`instances/<name>/instance.json`): which plugin + its config + which dispatch group. |
| **Trigger bus** | Correlates events from multiple sources (e.g. 6 cameras + a PLC trigger) into one inspection event, by trigger id within a window. |
| **Dispatch group** | A pool of worker threads (its own `max_parallel`, OS priority, cpu_affinity, queue, rate limit, ordering) that runs inspections. Groups are isolated. |
| **Run-result** | The one verdict per run: a signed status code + message (`>0` ok, `0` NA, `-1…` ng, `≤-990000` framework system-fail). Feeds the HMI + PLC. |
| **HMI** | A standalone browser SPA (cards on a split-pane dashboard) that connects to the backend over WebSocket and *only* visualizes. |

The full glossary is at the bottom (§9).

---

## 3. The architecture in one picture

```
                 ┌────────────────────────── one machine ──────────────────────────┐
                 │  xinsp-fe.exe ──spawns──►  xinsp-backend.exe  ──WS(:7823)──► HMI   │
   PLC / line ◄──┤  (supervisor,              (compute core:        (browser SPA,     │
   (safe-state)  │   safe-state,               trigger bus +         viewer only)     │
                 │   respawn; ROOT)            dispatch groups +                      │
                 │                             the script + plugins,                  │
                 │                             all in-process)                        │
                 └───────────────────────────────────────────────────────────────────┘

   PLC I/O is a plugin concern (a comm plugin owns the socket); the FE forwards a
   plugin-registered payload to the PLC on a backend crash (safe-state).

   Dev box: the VS Code extension plays the FE's role (spawns/attaches the BE),
            plus the edit→compile→run loop, instance/plugin trees, and webviews.
```

Two executables + two front-ends:

- **`xinsp-backend.exe` (BE)** — the compute core. Opens a project, compiles the
  script + plugins, runs continuous dispatch, serves a **WebSocket** API. All
  plugins run *in-process* (a hard plugin crash takes the BE down — that's the
  accepted trade for zero-copy speed; the FE is the safety net).
- **`xinsp-fe.exe` (FE)** — the **supervisor / root** in production. Spawns +
  monitors the BE, drives **safe-state** on a backend death (forwarding a
  plugin-registered PLC payload via `host->set_safe_state`), rate-limited respawn.
  See [`design/fe-be-split.md`](./design/fe-be-split.md). PLC I/O itself is a
  plugin — see [`design/comms-gateway.md`](./design/comms-gateway.md).
- **VS Code extension** (`vscode-extension/`) — the dev front-end: spawn/attach the
  BE, edit→run, instance/plugin trees, viewer, Project Settings webview.
- **HMI** (`hmi/`) — the production operator panel: a static browser SPA, WS client
  only. See [`design/production-hmi.md`](./design/production-hmi.md).

Who-may-crash, who-knows-the-project-folder, and the production boot order are in
[`design/deployment.md`](./design/deployment.md) — read it before deploying.

---

## 4. The life of one inspection (data flow)

1. A **source** (camera plugin / PLC-trigger bridge / any self-emitting plugin)
   calls `emit_trigger(...)` tagged with a trigger id + its **dispatch group**
   (from `instance.json`).
2. The **trigger bus** correlates required sources by id within a window
   (`trigger_policy`), producing one event carrying all the images.
3. The event is routed to its **group's lane** and run by a worker thread (up to
   `max_parallel` in parallel).
4. The **script** `xi_inspect_entry()` runs: reads the trigger images
   (`xi::current_trigger()`), calls plugins (`xi::use("det").process(...)` → a
   `Record`), computes a verdict, and emits:
   - `VAR(...)` / `EMIT(...)` — per-run inspection values (debug/detail),
   - `xi::result(code, msg)` — the **one verdict** (run-result),
   - optionally forwards to the PLC via a comm plugin (`xi::use("comm")...`).
5. Emission is gated per group (`result_order:"arrival"` keeps the stream in frame
   order even under parallel compute).
6. The **HMI** (WS client) renders the vars + verdict; the **PLC** gets the verdict
   to fire sort valves. Same run-result, multiple consumers.

The two ways continuous mode is *driven* (real triggers vs the source-less
synthetic timer tick) are spelled out at the top of `backend/src/service_main.cpp`
and in [`protocol.md`](./protocol.md) `cmd:start` — don't conflate them.

---

## 5. First day: build, run, change

**Prerequisites** (one-time): see [`guides/install.md`](./guides/install.md) —
MSVC Build Tools + OpenCV are required; turbojpeg/IPP optional. `tools/setup-windows.ps1`
is the one-click installer.

**Build the backend + tools:**
```powershell
cmake --build backend/build --config Release
```
(Outputs land in `backend/build/Release/`: `xinsp-backend.exe`, `xinsp-fe.exe`,
the test exes, + the runtime DLLs.)

**Run a project the fast way (VS Code):** open the repo in VS Code with the
xInsp2 extension, open a project under `examples/`, hit compile/run. The extension
spawns the BE, compiles the script, and streams vars to the viewer. See
[`guides/extending-the-ui.md`](./guides/extending-the-ui.md) for what the
extension can do.

**Run a project headless (what FE/production does):**
```powershell
backend/build/Release/xinsp-backend.exe --project=examples/qa_group_parallelism --autostart-fps=-1
```
(`--autostart-fps=-1` = trigger-only; the project's sources drive it.)

**See the HMI:** `node hmi/serve.mjs` (serves the SPA + proxies the WS) then open
the printed URL; or open `hmi/index.html?ws=ws://127.0.0.1:7823/` directly. See
[`hmi/README.md`](../hmi/README.md).

**Make your first change:** the smallest end-to-end loop is editing a project's
`inspect.cpp` and re-running — the backend hot-reloads the DLL. Try changing a
`VAR(...)` or a `xi::result(...)` in `examples/qa_run_result/inspect.cpp`.

**Run the test suite:**
```powershell
python tools/run_qa.py            # all examples/qa_*/driver.py (~6 min)
python tools/run_qa.py group      # just the dispatch-group regressions
```
ctest covers the C++ unit tests (`backend/tests/`). See [`testing.md`](./testing.md).

---

## 6. "I want to ___" — guided index

| Goal | Start here |
|---|---|
| Set up a build machine | [`guides/install.md`](./guides/install.md) |
| Write the inspection script | [`guides/writing-a-script.md`](./guides/writing-a-script.md) (`xi::use` / `VAR` / `xi::result` / triggers / params) |
| Add a plugin (camera/detector/op/saver) | [`guides/adding-a-plugin.md`](./guides/adding-a-plugin.md) + [`reference/plugin-abi.md`](./reference/plugin-abi.md) + [`reference/host_api.md`](./reference/host_api.md) |
| Understand instances / config | [`reference/instance-model.md`](./reference/instance-model.md) |
| Tune parallelism / priority / affinity / ordering / rate | [`design/dispatch-groups.md`](./design/dispatch-groups.md) (cheat-sheet at the end) — also editable in the Project Settings UI |
| Understand the per-run verdict | [`design/run-result.md`](./design/run-result.md) |
| Build the operator HMI | [`design/production-hmi.md`](./design/production-hmi.md) + [`hmi/README.md`](../hmi/README.md) |
| Talk to a PLC | [`design/comms-gateway.md`](./design/comms-gateway.md) |
| The supervisor / crash-safety | [`design/fe-be-split.md`](./design/fe-be-split.md) |
| Deploy to a target PC (no toolchain) | [`design/deployment.md`](./design/deployment.md) — `tools/export_bundle.py` |
| Extend the VS Code extension | [`guides/extending-the-ui.md`](./guides/extending-the-ui.md) |
| The WS wire format | [`protocol.md`](./protocol.md) |
| Debug a crash | [`guides/debugging.md`](./guides/debugging.md) |
| Image I/O / formats | [`reference/image-io.md`](./reference/image-io.md) |
| Add/run tests | [`testing.md`](./testing.md) |
| The full technical reference | [`architecture.md`](./architecture.md) |

---

## 7. Codebase layout

```
backend/
  src/           service_main.cpp (BE — the big one), fe_main.cpp (supervisor),
                 runner_main.cpp (headless runner), xi_image_io.cpp
  include/xi/    the SDK headers (50+): xi_plugin_manager, xi_trigger_bus,
                 xi_image_pool, xi_script_*, xi_use, xi_result, xi_types, …
  tests/         C++ unit tests (ctest) + benchmarks
  CMakeLists.txt build (targets: xinsp_backend / xinsp_fe / xinsp_runner / tests)
hmi/             the operator SPA (app.mjs, cards.mjs, layout.mjs, protocol.mjs,
                 serve.mjs, dashboard*.json)
vscode-extension/ the dev front-end (src/extension.ts + modules)
examples/        runnable projects + the qa_*/driver.py regression suite
plugins/         globally-discoverable plugins
tools/           export_bundle.py, run_qa.py, setup-windows.ps1, xinsp2_py/ (Python SDK)
docs/            you are here
```

The single most important source file is **`backend/src/service_main.cpp`** — the
BE: command handling, dispatch pools, the script lifecycle, crash filter. Skim its
top (globals + the "CONTINUOUS RUN HAS TWO DRIVERS" comment) to orient.

---

## 8. Conventions worth knowing on day one

- **Windows-first, but keep new code cross-platform:** gate Win32 with `#ifdef
  _WIN32` + a `// TODO(linux):` and record it in
  [`design/linux-port.md`](./design/linux-port.md).
- **Docs ride with code:** a behavior change updates its matching doc in the same
  commit (see [`README.md`](./README.md) "When to update what").
- **`status.md` is the single source of truth** for what ships — no parallel
  STATUS/DEV_PLAN files to drift against.
- **Tests are `examples/qa_*/driver.py`** (Python, drive a real backend) +
  `backend/tests/test_*.cpp` (C++ unit). Run via `tools/run_qa.py` / ctest.
- **`VAR(name, expr)` declares a local; `EMIT(name)` surfaces an existing one** —
  `VAR(x, x)` is a redefinition error (C2374). Common first-day gotcha.

---

## 9. Glossary

- **BE / FE** — `xinsp-backend.exe` (compute) / `xinsp-fe.exe` (supervisor). PLC
  I/O is a plugin; the FE forwards a plugin-registered payload to the PLC on crash.
- **trigger / trigger id** — an event from a source; the id is the correlation key
  (e.g. the turntable index / frame sequence).
- **trigger_policy** — `any` / `all_required` / `leader_followers` + a window: how
  the bus correlates multi-source events.
- **dispatch group** — a per-group worker lane (`max_parallel`, `thread_priority`,
  `cpu_affinity`, `queue_depth`/`overflow`, `min_interval_ms`, `result_order`).
- **trigger-only** — continuous mode with no synthetic timer tick (`fps<=0`);
  sources are the sole driver. The production path.
- **run-result** — the one signed verdict code + message per run (`run_result`
  event). System-fail band `≤-990000` is framework-only (e.g. `XI_SYS_DROPPED`).
- **AOT bundle** — a self-contained export (`tools/export_bundle.py`) the target
  runs with `--aot` (prebuilt script/plugin DLLs, no toolchain).
- **working copy** — a transactional `<project>/.xinsp_work` scratch so edits
  survive a crash-respawn ([`guides/project-working-copy.md`](./guides/project-working-copy.md)).
- **ImagePool** — the host-side lock-free refcounted image pool; images move by
  handle, zero-copy.

---

Welcome aboard. When in doubt: the [doc index](./README.md) maps tasks → docs, and
[`architecture.md`](./architecture.md) has the exhaustive reference.
