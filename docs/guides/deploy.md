# Deployment topology — boot order, ownership, and the export bundle

> How an xInsp2 line runs on a production machine: who launches what, who may
> crash, and who knows the project folder. Pairs with
> [`fe-be.md`](../internals/fe-be.md) (FE supervisor). PLC I/O — and the
> "BE crashed → drive the line safe" action — are a comms plugin's job (its own
> sidecar process), NOT the FE/core.

## Boot order — FE is the root, not the HMI

```
OS boot / login
  └─ Windows Service (SCM) or Task Scheduler / a Job object        ← OS owns the root
        └─ xinsp-fe.exe        (the ONLY root; supervisor: monitor + respawn)
              └─ spawn xinsp-backend.exe --project=<DIR> --autostart-fps=N|-1
                      └─ BE self-opens the project → compiles/loads → start (continuous)
        FE holds the BE in a Job Object (no orphans), monitors, respawns, and
        records crash history. "Go safe on a BE crash" is a comms plugin's own
        sidecar process (it watches the BE and signals the PLC on death) — not FE.

[separate, NON-critical]  operator panel
  └─ kiosk browser (msedge/chrome --kiosk --app=<url>) or an Electron shell
        └─ loads the HMI static artifact + WS-connects to ws://127.0.0.1:<be-port>
```

**Why FE is root, not Electron/HMI:** FE is the supervisor — it must be the most
resilient process. The HMI is the *least* critical layer (just a viewer); it must
never sit in the critical path. Putting a heavy, crash-prone Chromium at the root
would gamble the whole line on a browser shell.

### Who may die

| Process | May crash? | Recovery |
|---|---|---|
| **FE** | no (it's the root) | OS service restarts it — last-resort backstop |
| **BE** | yes | FE catches → crash-history + rate-limited respawn (line-safe is the comms plugin's sidecar) |
| **HMI / kiosk** | freely | reopen the browser; inspection keeps running |

> **Clean shutdown on abrupt exit.** The BE installs a console-control handler, so
> closing its console window, `Ctrl+C`/`Ctrl+Break`, logoff, and system shutdown all
> run the **same controlled teardown** as `cmd:shutdown` (plugin destructors fire —
> e.g. a comm/PLC plugin's "go-safe on close" — and no spurious crash minidump is
> written). For the window-close/logoff/shutdown class the OS grants only a short
> grace window (~5 s): a clean teardown completes within it in the normal case, but
> if an inspect is wedged the process still hard-exits (the FE respawns) rather than
> hang. This is orderly-exit best-effort, not a guarantee — the comms sidecar
> remains the line-safe backstop.

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
`qa/qa_export_bundle/` (runs the bundle with a stripped PATH — no `cl.exe`
reachable — and still inspects).

```
<out>/
  bin/        xinsp-fe.exe, xinsp-backend.exe + all runtime DLLs
              (OpenCV, accelerators, the VC++ redist DLLs)
  project/    project.json, instances/, plugins/<p>/{plugin.json, *.dll prebuilt},
              inspect.cpp + the PREBUILT script DLL, dashboard.json
  hmi/        the static HMI artifact (index.html, *.mjs)
  fe.json     launch config (project=./project, respawn)
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
| **Open** | `open_project { "path": DIR, "working_copy": true }` | If `.xinsp_work` exists → **resume** it (crash recovery / unsaved session). Else → **seed** it from the canonical project. The BE then runs entirely on the scratch. **If the seed copy fails** (disk full, a locked/permission-denied file), the open is **aborted** with an error and the partial scratch is **removed** — a torn seed that merely happens to carry `project.json` must never be accepted as authoritative, because the eventual `commit_working_copy` mirror would then **prune** the canonical files that only failed to copy in (silent data loss). Removing the partial scratch also stops a later crash-resume from adopting it. |
| **Save Project** | `commit_working_copy` | Drains the dispatch pool first (same as `discard`/`open` — the mirror touches files continuous workers are reading), then mirrors the scratch back onto the canonical project — adds, overwrites, **and deletes** files removed in the scratch (so e.g. a deleted instance propagates). The commit is journalled: a `.xinsp_commit_pending` marker is written at the canonical root before the mirror and removed after. If a crash/power-loss interrupts the mirror (leaving the canonical torn), the marker survives, and the next `open_project` rolls the commit **forward** — the scratch is never modified by a commit, so it's a complete snapshot, and the mirror is idempotent, so re-running it heals the canonical. |
| **Discard changes** | `discard_working_copy` | Delete the scratch, re-seed from the canonical project, reopen. Throws away uncommitted edits. **Crash-recovery guard:** if a `.xinsp_commit_pending` marker is present (a prior `commit_working_copy` was interrupted, leaving the canonical possibly torn), Discard **completes that commit first** — it rolls the scratch forward onto the canonical (the same heal `open_project` does) and clears the marker **before** removing the scratch. A pending commit is *not* "uncommitted edits"; it's a half-applied commit the user already requested, and the intact scratch is the only thing that can heal the torn canonical (rolling *back* is impossible — the canonical bytes are already partly overwritten). So Discard never leaves the canonical torn. If the heal mirror fails (persistent disk error), the scratch + marker are **kept** for a later retry rather than discarded. |

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
`qa/qa_working_copy/driver_fe.py` (FE seeds the scratch, an edit is saved,
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

Regression: `qa/qa_working_copy/` drives the whole lifecycle (seed →
isolation → crash-resume → commit → discard) against an on-disk `kv` instance.
