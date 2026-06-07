# Project working copy (transactional edits + crash-durable)

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

## Lifecycle

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

## Headless / FE-supervised

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

## Notes & current limits

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
