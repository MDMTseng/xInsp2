# xInsp2 documentation

> **Scope:** one-page index — pick the entry closest to your task.
> **Status:** SKELETON (docs-v2 build, see [`PLAN.md`](./PLAN.md)). Content is
> being ported from `docs/`, which stays authoritative until cutover.

New here? Read [`overview.md`](./overview.md) first (~20 min).

## Understand
| File | Question it answers |
|---|---|
| [`overview.md`](./overview.md) | What is this? Mental model, architecture-in-one-picture, the core nouns. |

## Do (`guides/`)
| Guide | When to read |
|---|---|
| [`guides/build-and-run.md`](./guides/build-and-run.md) | Set up a machine, build, run on day one. |
| [`guides/write-a-script.md`](./guides/write-a-script.md) | Write the inspection script for a project. |
| [`guides/write-a-plugin.md`](./guides/write-a-plugin.md) | Author a plugin (in-project + standalone) + its UI. |
| [`guides/debug.md`](./guides/debug.md) | Something crashed — what's caught, crash reports, attach a debugger. |
| [`guides/extend-the-ui.md`](./guides/extend-the-ui.md) | Add a command / tree / webview to the VS Code extension. |
| [`guides/deploy.md`](./guides/deploy.md) | Production: boot order, AOT bundle, working-copy edits. |

## Exact contracts (`reference/`)
| Reference | Subject |
|---|---|
| [`reference/c-abi.md`](./reference/c-abi.md) | Plugin DLL exports + the `xi_host_api` service table — one ABI doc. |
| [`reference/data-types.md`](./reference/data-types.md) | What crosses the boundary: Record + Image + typed I/O + NA. |
| [`reference/ws-protocol.md`](./reference/ws-protocol.md) | WebSocket commands / replies / events / binary frames. |
| [`reference/instances.md`](./reference/instances.md) | Instance load / persist / registry / teardown + `instance.json`. |

## How it works inside (`internals/`) — SHIPPED design-of-record
| File | Subsystem |
|---|---|
| [`internals/data-layer.md`](./internals/data-layer.md) | yyjson-only + in-process doc pass-by-pointer + γ-4 cross-ABI refcount. |
| [`internals/dispatch.md`](./internals/dispatch.md) | How a trigger becomes a run: trigger bus + emit/fetch + dispatch groups. |
| [`internals/fe-be.md`](./internals/fe-be.md) | FE supervisor over the BE compute core, safe-state, crash history. |
| [`internals/typed-io.md`](./internals/typed-io.md) | Nominal types over Record + NA propagation + provenance. |

## Planned / not scheduled (`roadmap/`)
| File | Subject |
|---|---|
| [`roadmap/README.md`](./roadmap/README.md) | Shipped-status summary + the roadmap (links, doesn't restate). |
| `roadmap/run-result.md` · `interactive-tool-registry.md` · `production-hmi.md` · `linux-port.md` | Forward-looking sketches; graduate to `internals/` on ship. |

## Archive
Historical snapshots + removed subsystems (SHM, comms-gateway, review snapshots).
Populated at cutover.
