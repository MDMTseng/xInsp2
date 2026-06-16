# Deployment topology — boot order, ownership, and the export bundle

> How an xInsp2 line runs on a production machine: who launches what, who may
> crash, and who knows the project folder. Pairs with
> [`fe-be.md`](../internals/fe-be.md) (FE supervisor). PLC I/O is a plugin
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

`python tools/export_bundle.py <project> <out> [--fps N] [--port P]` produces a self-contained
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
[`guides/build-and-run.md`](../guides/build-and-run.md) for the dev-box toolchain that export
itself needs.


---

## Transactional project edits (working copy)

> **Status: BE core + FE auto-resume shipped (Increments 1–2).** The VS Code
> "Save Project / Discard" UI (Inc 3) builds on this.

When a project is opened in **working-copy mode**, the backend operates on a
scratch copy at `<project>/.xinsp_work` instead of the project itself. Every
edit — instance configs, params, project.json, and (full copy) plugin/script
source — lands in the scratch. The canonical project on disk is **untouched
until an explicit commit**. Because the scratch is on disk, it also survives a
backend crash: on respawn the backend **resumes** it, so in-progress settings
aren't lost.

```
<project>/                     <- canonical (changes only on commit)
  project.json
  instances/<name>/instance.json
  plugins/<name>/...
  inspect.cpp
  .gitignore                   <- ".xinsp_work/" auto-added
  .xinsp_work/                 <- working copy (the BE actually runs on this)
    project.json   instances/   plugins/   inspect.cpp
```

### Lifecycle

| Action | Command | Effect |
|---|---|---|
| **Open** | `open_project { "path": DIR, "working_copy": true }` | If `.xinsp_work` exists → **resume** it (crash recovery / unsaved session). Else → **seed** it from the canonical project. The BE then runs entirely on the scratch. |
| **Save Project** | `commit_working_copy` | Drains the dispatch pool first (same as `discard`/`open` — the mirror touches files continuous workers are reading), then mirrors the scratch back onto the canonical project — adds, overwrites, **and deletes** files removed in the scratch (so e.g. a deleted instance propagates). The commit is journalled: a `.xinsp_commit_pending` marker is written at the canonical root before the mirror and removed after. If a crash/power-loss interrupts the mirror (leaving the canonical torn), the marker survives, and the next `open_project` rolls the commit **forward** — the scratch is never modified by a commit, so it's a complete snapshot, and the mirror is idempotent, so re-running it heals the canonical. |
| **Discard changes** | `discard_working_copy` | Delete the scratch, re-seed from the canonical project, reopen. Throws away uncommitted edits. |

`open_project`'s response carries `"working_copy": true`, `"canonical_path"`, and
`"working_dir"` so a client (the VS Code extension) can target the scratch for
editing and show a "save" affordance.

**What's excluded** from seed/commit: the `.xinsp_work` dir itself, `.git`, and
any `build/` directory (plugin DLLs are recompiled in the scratch on open;
committing them back would clobber the canonical build).

### Headless / FE-supervised

Pass **`--working-copy`** alongside `--project` to autostart in working-copy mode:

```
xinsp-backend --port=7823 --project=C:\line\proj --working-copy --autostart-fps=10
```

On a crash, the FE supervisor respawns the backend with the **same** flags; the
`.xinsp_work` scratch still exists, so the backend resumes the last in-progress
settings rather than reverting to the pristine project — settings survive the
crash. The FE forwards `--working-copy` (CLI or `fe.json` `"working_copy": true`)
to every backend it spawns, including respawns. Proven by
`examples/qa_working_copy/driver_fe.py` (FE seeds the scratch, an edit is saved,
the backend is hard-killed, the FE respawns it and the backend resumes the
scratch with the uncommitted edit intact).

### Notes & current limits

- The scratch lives **inside** the project (`.xinsp_work/`), so it persists
  across reboots and is auto-added to the project's `.gitignore`.
- Resume-if-present means a normal reopen of a project with an unsaved scratch
  **resumes** that scratch (you don't lose work). Use `discard_working_copy` to
  start clean from the canonical project.
- Full-copy mode assumes the editor and backend share a filesystem (the VS Code
  workspace edits the scratch files). A truly remote backend would need the
  scratch on the backend host — out of scope today.

Regression: `examples/qa_working_copy/` drives the whole lifecycle (seed →
isolation → crash-resume → commit → discard) against an on-disk `kv` instance.
