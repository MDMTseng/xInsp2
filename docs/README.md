# xInsp2 documentation

> **Scope:** one-page index — pick the entry closest to your task. Each fact has
> exactly one home; see [When to update what](#when-to-update-what) below.

New here? Read [`overview.md`](./overview.md) first (~20 min). **Want to hack on
xInsp2 itself?** Jump to [Develop xInsp2](#develop-xinsp2-itself) below.

## Understand
| File | Question it answers |
|---|---|
| [`overview.md`](./overview.md) | What is this? Mental model, architecture-in-one-picture, the core nouns. |
| [`repository-map.md`](./repository-map.md) | What's in the repo? The four products, per-package ownership / version / build / test / outputs. |

## Do (`guides/`)
| Guide | When to read |
|---|---|
| [`guides/build-and-run.md`](./guides/build-and-run.md) | Set up a machine, build, run on day one. |
| [`guides/write-a-script.md`](./guides/write-a-script.md) | Write the inspection script for a project. |
| [`guides/instance-lifecycle.md`](./guides/instance-lifecycle.md) | Operator tour: plugin folder → create instance → use from script → delete. |
| [`guides/write-a-plugin.md`](./guides/write-a-plugin.md) | Author a plugin (in-project + standalone) + its UI. |
| [`guides/plugin-ui.md`](./guides/plugin-ui.md) | Build a plugin's UI with the xi-components web components (playground, wiring, teach UIs, auto-panels). |
| [`guides/plugin-caveats.md`](./guides/plugin-caveats.md) | The non-obvious gotchas to skim before shipping a plugin (concurrency, prepare/commit, UI patterns). |
| [`guides/debug.md`](./guides/debug.md) | Something crashed — what's caught, crash reports, attach a debugger. |
| [`guides/extend-the-ui.md`](./guides/extend-the-ui.md) | Add a command / tree / webview to the VS Code extension. |
| [`guides/deploy.md`](./guides/deploy.md) | Production: boot order, AOT bundle, working-copy edits. |

## Exact contracts (`reference/`)
| Reference | Subject |
|---|---|
| [`reference/c-abi.md`](./reference/c-abi.md) | Plugin DLL exports + the `xi_host_api` service table + the `get_interface` planes (`xi.pack@1`, the capability plane) + optional script exports — one ABI doc. |
| [`reference/data-types.md`](./reference/data-types.md) | What crosses the boundary: Record + Image + typed I/O + NA. |
| [`reference/ws-protocol.md`](./reference/ws-protocol.md) | WebSocket commands / replies / events / binary frames. |
| [`reference/instances.md`](./reference/instances.md) | Instance load / persist / registry / teardown + `instance.json`. |

## How it works inside (`internals/`) — SHIPPED design-of-record
| File | Subsystem |
|---|---|
| [`internals/data-layer.md`](./internals/data-layer.md) | yyjson-only + in-process doc pass-by-pointer + γ-4 cross-ABI refcount (Record-era; the Pack plane is the row below). |
| [`internals/pack-plane.md`](./internals/pack-plane.md) | The v3 Pack data currency: container memory model, seal/registry/owner sweep, `$fault`/provenance contract, dual-carry dispatch, ingress, XEX1-v3. |
| [`internals/dispatch.md`](./internals/dispatch.md) | How an emit becomes a run: trigger bus + `emit_record`/`emit_pack` + dispatch groups + staged sinks. |
| [`internals/fe-be.md`](./internals/fe-be.md) | FE supervisor over the BE compute core, crash history. |
| [`internals/comms-sidecar.md`](./internals/comms-sidecar.md) | Line safety as a comms plugin's own sidecar process (replaces FE PLC safe-state). |
| [`internals/typed-io.md`](./internals/typed-io.md) | Typed, compile-checked keyed reads on the pack plane (`_keys.gen.h` constants + script-side `ScriptTypedPack`) + the contract codegen; the retired Record typed-I/O layer kept as labeled history. |

## Planned / not scheduled (`roadmap/`)
| File | Subject |
|---|---|
| [`roadmap/README.md`](./roadmap/README.md) | Shipped-status summary + the roadmap (links, doesn't restate). |
| `roadmap/run-result.md` · `interactive-tool-registry.md` · `production-hmi.md` · `config-bundles-and-orchestration.md` · `linux-port.md` | Forward-looking sketches; graduate to `internals/` on ship. |

## Develop xInsp2 itself
For working **on** the framework (not just authoring a project on it):

| Doc | What it covers |
|---|---|
| [`../CONTRIBUTING.md`](../CONTRIBUTING.md) | The "where do I start" doc: prerequisites, first build (backend **and** extension), the full pre-push test sweep, branch / commit / PR / coding style, doc culture. |
| [`guides/build-and-run.md`](./guides/build-and-run.md) | Toolchain setup, build from source, and the edit→run dev loop in detail. |
| [`testing.md`](./testing.md) | The whole test surface — C++ unit (`ctest`), Node WS suites, VS Code E2E, the `qa/qa_*` drivers — and how to add a test. |
| [`guides/write-a-plugin.md`](./guides/write-a-plugin.md) | Authoring, building (in-project + standalone `cmake`), UI-testing, and exporting a plugin. |
| [`guides/deploy.md`](./guides/deploy.md) | The `--aot` export bundle (`tools/export_bundle.py`) — ship to a PC with no compiler. |

## Archive
[`archive/`](./archive/) — historical snapshots + removed subsystems (SHM,
comms-gateway, architecture reviews). Kept for provenance, not maintained.

## When to update what

Doc and code ship in the **same commit**. The one-home rule means each fact lives
in exactly one file, so a change has exactly one doc to touch — find the row, edit
that file. (No `status.md` ↔ `architecture.md` re-statement to keep in sync.)

| You changed | Update |
|---|---|
| A backend WS command's args / reply / event | `reference/ws-protocol.md` |
| A plugin DLL export or an `xi_host_api` entry | `reference/c-abi.md` |
| What crosses the boundary (Record / Image / typed I/O / NA) | `reference/data-types.md` (+ `internals/typed-io.md` if it's the mechanics) |
| Instance load / persist / `instance.json` | `reference/instances.md` |
| The pack plane (container / door / `$`-key contract / ingress / XEX1) | `internals/pack-plane.md` (+ `reference/c-abi.md` §6 if the vtable changed) |
| A data-layer / dispatch / FE-BE internal | the matching `internals/*.md` |
| A user-facing workflow (build, script, plugin, debug, UI, deploy) | the matching `guides/*.md` |
| Milestone shipped / spike merged | `roadmap/README.md` (the status table) |
| Test layout / a new suite | `testing.md` |
| A new, not-yet-shipped design | a `roadmap/*.md` sketch; graduate it to `internals/` on ship |
