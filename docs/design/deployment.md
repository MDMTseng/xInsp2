# Deployment topology — boot order, ownership, and the export bundle

> How an xInsp2 line runs on a production machine: who launches what, who may
> crash, and who knows the project folder. Pairs with
> [`fe-be-split.md`](./fe-be-split.md) (FE supervisor). PLC I/O is a plugin
> concern; BE-death → safe-state is the FE's job (`host->set_safe_state`).

## Boot order — FE is the root, not the HMI

```
OS boot / login
  └─ Windows Service (SCM) or Task Scheduler / a Job object        ← OS owns the root
        └─ xinsp-fe.exe        (the ONLY root; supervisor + safe-state driver)
              └─ spawn xinsp-backend.exe --project=<DIR> --autostart-fps=N|-1
                      └─ BE self-opens the project → compiles/loads → start (continuous)
        FE holds the BE in a Job Object (no orphans), monitors, respawns, and
        drives PLC safe-state on a backend death (forwarding a plugin-registered
        payload via host->set_safe_state). PLC I/O is otherwise a plugin concern.

[separate, NON-critical]  operator panel
  └─ kiosk browser (msedge/chrome --kiosk --app=<url>) or an Electron shell
        └─ loads the HMI static artifact + WS-connects to ws://127.0.0.1:<be-port>
```

**Why FE is root, not Electron/HMI:** FE is the supervisor + the safe-state driver
— it must be the most resilient process. The HMI is the *least* critical layer
(just a viewer); it must never sit in the critical path. Putting a heavy, crash-
prone Chromium at the root would gamble the whole line on a browser shell. The
PLC safe-state path (BE death → FE → PLC, plugin-owned) is independent of whether
the HMI is even running.

### Who may die

| Process | May crash? | Recovery |
|---|---|---|
| **FE** | no (it's the root) | OS service restarts it — last-resort backstop |
| **BE** | yes | FE catches → safe-state → rate-limited respawn |
| **HMI / kiosk** | freely | reopen the browser; inspection + safety keep running |

## Who knows the project folder

The project path is configured **once** (deployment layer) and flows **down** to BE
and stops there:

```
fe.json "project"  (or launcher --project)   ← single source of truth
        ▼
   xinsp-fe.exe ──spawn──► xinsp-backend.exe --project=<DIR>
                                 └─ the ONLY process that opens the folder
```

| Component | Needs the project folder? | How |
|---|---|---|
| **FE** | yes (decides which project) | `fe.json` `project` / launcher `--project` |
| **BE** | yes (the one that opens it) | FE passes `--project=<DIR>` |
| **HMI** | **no** | only the BE WS URL; it pulls the dashboard and data from BE |

The HMI's dashboard comes from the backend (`cmd:get_dashboard` →
`<project>/dashboard.json`), so the HMI is fully decoupled from the filesystem —
give it `ws://host:<be-port>/` and it's self-sufficient. Dev mirrors this: the VS
Code extension is the source (managed mode spawns BE with `--project`; attach mode
leaves it to FE). Rule everywhere: **whoever spawns BE passes it the project path.**

## Export bundle — copy to another PC, run with minimal requirements

`python tools/export_bundle.py <project> <out> [--fps N]` produces a self-contained
folder (✅ implemented). The **target machine needs no toolchain** — the script +
project plugins are **pre-compiled on the dev box** (via a throwaway dev backend)
and shipped as DLLs; the target backend loads them with **`--aot`** (a `.dll`
`script` path is loaded directly; project plugins load the newest `build/*.dll`
instead of compiling), so the target never runs `cl.exe`. Proven by
`examples/qa_export_bundle/` (runs the bundle with a stripped PATH — no `cl.exe`
reachable — and still inspects).

```
<out>/
  bin/        xinsp-fe.exe, xinsp-backend.exe + all runtime DLLs
              (OpenCV, accelerators, the VC++ redist DLLs)
  project/    project.json, instances/, plugins/<p>/{plugin.json, *.dll prebuilt},
              inspect.cpp + the PREBUILT script DLL, dashboard.json
  hmi/        the static HMI artifact (index.html, *.mjs)
  fe.json     launch config (project=./project, safe-state, respawn)
  run.bat     starts FE pointed at fe.json
  README.txt
```

**Target requirements:** Windows + (shipped) VC++ runtime DLLs. No MSVC, no OpenCV
install, no Node — the HMI runs in the machine's browser; the script/plugins are
prebuilt. The backend loads the prebuilt script DLL instead of compiling (so the
toolchain is a *build/export-time* dependency, not a *run-time* one). See
[`guides/install.md`](../guides/install.md) for the dev-box toolchain that export
itself needs.
