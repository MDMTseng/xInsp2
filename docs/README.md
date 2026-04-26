# xInsp2 documentation

This is the doc tree's index. Pick the entry closest to your task.

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
| [`guides/adding-a-plugin.md`](./guides/adding-a-plugin.md) | You want to add a camera / detector / saver / op. Both in-project (fast iteration) and standalone (distributable) paths covered. |
| [`guides/writing-a-script.md`](./guides/writing-a-script.md) | You're writing the inspection script for a project. Lifecycle + every primitive (`xi::use` / `xi::Param` / `VAR` / `xi::Record` / `xi::async` / `xi::state` / `xi::breakpoint` / triggers). |
| [`guides/debugging.md`](./guides/debugging.md) | Something crashed. What's caught, what isn't, how to read crash reports, how to attach a debugger. |
| [`guides/extending-the-ui.md`](./guides/extending-the-ui.md) | You're adding a command / tree item / webview / status bar element to the VS Code extension. Maps every common task to an existing example. |

## Reference (`reference/`)

Deep API surfaces. Look here when you need exact contract / argument
shapes.

| Reference | Subject |
|---|---|
| [`reference/host_api.md`](./reference/host_api.md) | The `xi_host_api` function table the host hands to every plugin |
| [`reference/plugin-abi.md`](./reference/plugin-abi.md) | The C exports a plugin DLL must provide; `XI_PLUGIN_IMPL` macro |
| [`reference/instance-model.md`](./reference/instance-model.md) | How instances are loaded, persisted, registered, destroyed; `instance.json` schema; isolation modes |
| [`reference/ipc-shm.md`](./reference/ipc-shm.md) | Cross-process isolation architecture (currently on `shm-process-isolation` spike branch) |

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
