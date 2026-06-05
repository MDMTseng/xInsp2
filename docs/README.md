# xInsp2 documentation

This is the doc tree's index. Pick the entry closest to your task.

> **New to the project? Start with [`getting-started.md`](./getting-started.md)** —
> the mental model, the architecture in one picture, a build-and-run-on-day-one
> path, and a guided index into everything below. ~30 minutes.

## Top-level

| File | Purpose |
|---|---|
| [`architecture.md`](./architecture.md) | Technical reference: components, data flow, lifecycles, file map, every API in detail |
| [`status.md`](./status.md) | What's currently shipping vs in flight — the single source of truth (no parallel `DEV_PLAN.md` / `STATUS.md` to drift against) |
| [`testing.md`](./testing.md) | Test layout, how to run, what each suite proves, how to add new tests |
| [`protocol.md`](./protocol.md) | WebSocket wire-format reference (commands, replies, events, binary preview frames) |

## Onboarding (`guides/`)

Task-shaped — pick the verb that matches what you're doing.

| Guide | When to read |
|---|---|
| [`guides/install.md`](./guides/install.md) | You're setting up a new Windows machine to build/run xInsp2. What's required (MSVC + OpenCV), what's optional (turbojpeg/IPP), and the in-editor C++ Toolchain health check + per-project path overrides. |
| [`guides/adding-a-plugin.md`](./guides/adding-a-plugin.md) | You want to add a camera / detector / saver / op. Both in-project (fast iteration) and standalone (distributable) paths covered. |
| [`guides/writing-a-script.md`](./guides/writing-a-script.md) | You're writing the inspection script for a project. Lifecycle + every primitive (`xi::use` / `xi::Param` / `VAR` / `xi::Record` / `xi::async` / `xi::state` / `xi::breakpoint` / triggers). |
| [`guides/debugging.md`](./guides/debugging.md) | Something crashed. What's caught, what isn't, how to read crash reports, how to attach a debugger. |
| [`guides/extending-the-ui.md`](./guides/extending-the-ui.md) | You're adding a command / tree item / webview / status bar element to the VS Code extension. Maps every common task to an existing example. |
| [`guides/plugin-ui-conventions.md`](./guides/plugin-ui-conventions.md) | You're writing a plugin's instance UI and want it automatable: the `data-param`/`data-action` selector convention, the `h.setParam`/`h.action` test helpers, and `useProjectPlugin` for UI-testing a source-only plugin. |
| [`guides/project-working-copy.md`](./guides/project-working-copy.md) | Transactional project edits: open on a `.xinsp_work` scratch, commit/discard, crash-durable resume. |

## Reference (`reference/`)

Deep API surfaces. Look here when you need exact contract / argument
shapes.

| Reference | Subject |
|---|---|
| [`reference/image-io.md`](./reference/image-io.md) | `xi::Image` / `xi::imread` / OpenCV interop (`as_cv_mat`, `from_cv_mat`), the RGB-not-BGR gotcha, and what `VAR` can track |
| [`reference/host_api.md`](./reference/host_api.md) | The `xi_host_api` function table the host hands to every plugin |
| [`reference/plugin-abi.md`](./reference/plugin-abi.md) | The C exports a plugin DLL must provide; `XI_PLUGIN_IMPL` macro |
| [`reference/instance-model.md`](./reference/instance-model.md) | How instances are loaded, persisted, registered, destroyed; `instance.json` schema; isolation modes |
| [`reference/ipc-shm.md`](./reference/ipc-shm.md) | Cross-process isolation architecture (currently on `shm-process-isolation` spike branch) |

## Design sketches (`design/`)

Forward-looking design docs for work that isn't implemented yet.
Updated when scope solidifies; deleted when the work lands and the
content moves into the relevant reference / guide.

| File | Subject |
|---|---|
| [`design/fe-be-split.md`](./design/fe-be-split.md) | The frontend supervisor (`xinsp-fe.exe`) over the in-process backend compute core: process lifecycle, the `SafeStateSink` PLC seam, backend headless-autostart flags, and the VS Code managed/attach modes. |
| [`design/fe-be-split-test-plan.md`](./design/fe-be-split-test-plan.md) | Test plan for the FE/BE split: unit / integration / e2e / safety-property coverage, regression gates, and the priority gaps to close. |
| [`design/comms-gateway.md`](./design/comms-gateway.md) | Design (not built): out-of-process comms/I/O plugins — a standalone "comms gateway" process for the evolving PLC interface, isolated from the FE and BE; the safe-state boundary stays direct. |
| [`design/interactive-tool-registry.md`](./design/interactive-tool-registry.md) | How plugins that need GUI setup (shape-model template editing, ROI mask drawing) hook into a shared image-viewer panel without reimplementing pan/zoom/draw plumbing. |
| [`design/linux-port.md`](./design/linux-port.md) | Cross-platform port (Linux / ARM / macOS): Windows-only inventory, effort + phasing estimates, ARM/macOS deltas, the runtime-compile / AOT-bundle strategy, and the going-forward rule that new code stays cross-platform-friendly. Not scheduled. |
| [`design/dispatch-groups.md`](./design/dispatch-groups.md) | Priority + concurrency for trigger work: each dispatch group owns its `max_parallel` worker threads at its OS `thread_priority` (+ per-group queue/rate). No shared pool / priority queue — group priority = thread_priority + max_parallel, and the OS preempts on the cores. Default = high(4)/low(1); legacy (no groups) = one implicit group. Design, not scheduled. |
| [`design/deployment.md`](./design/deployment.md) | Production deployment: boot order (OS service → FE root → BE + comms; HMI is a non-critical viewer), who may crash, who knows the project folder (FE + BE only), and the **AOT export bundle** (`tools/export_bundle.py` → copy-and-run folder, `--aot` loads prebuilt script/plugin DLLs so the target needs no toolchain). |
| [`design/run-result.md`](./design/run-result.md) | Per-run result record (one per trigger): a signed status code (`>0` ok-class, `0` NA, `-1…` user ng-class, `≤ -990000` a framework system-fail enum: dropped/crashed/timeout/no-verdict) + message + provenance. The framework fills the non-run cases (drop → `XI_SYS_DROPPED`) so the result stream has no silent gaps; feeds the HMI verdict/yield/Pareto cards + the comms/PLC verdict. `RESULT(code,msg)` API, distinct from `VAR`. Design, not scheduled. |
| [`design/production-hmi.md`](./design/production-hmi.md) | Production operator HMI + standalone package export: a recursive split-pane SPA dashboard composer (compose/run modes), script-computes-data binding, web-component cards, vector overlay layers, the plugin `ui`/`card`/`overlay` surfaces, `dashboard.json`, and the AOT production bundle. v1.0 RUN mode built under `hmi/`. |

## Archive (`archive/`)

Historical snapshots — kept for context, not for planning.

| File | Why we keep it |
|---|---|
| [`archive/newdeal-M0.md`](./archive/newdeal-M0.md) | Original M0 architectural vision; useful for understanding why specific choices were made |
| [`archive/test-audit-2026-04-15.md`](./archive/test-audit-2026-04-15.md) | A snapshot of bug-coverage gaps as of mid-April 2026; resolved findings logged in commit history |

## Adjacent

- **Repo root [`README.md`](../README.md)** — the elevator pitch +
  install / first-use walkthrough.
- **[`CONTRIBUTING.md`](../CONTRIBUTING.md)** — environment setup,
  branch policy, commit style.
- **[`sdk/README.md`](../sdk/README.md)** — plugin SDK reference.
- **[`sdk/GETTING_STARTED.md`](../sdk/GETTING_STARTED.md)** — SDK
  5-minute walkthrough.
- **[`tools/xinsp2_py/README.md`](../tools/xinsp2_py/README.md)** —
  Python WS client for AI-driven inspection workflows. Pairs with the
  `.claude/skills/xinsp2/` skill.

---

## When to update what

| You changed | Update |
|---|---|
| A backend cmd's args / reply | `protocol.md` |
| A plugin / script API | `architecture.md` + relevant guide + relevant reference |
| Test layout | `testing.md` |
| Milestone done / spike merged | `status.md` |
| New feature requiring a tutorial | new guide under `guides/` |
| New API requiring deep docs | new reference under `reference/` |

Doc and code in the same commit is the standard. Doc-only commits are
fine when they're catching up — see this branch (`doc-cleanup`) for
the canonical example.
