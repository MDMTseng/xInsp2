# Config bundles, object/version switching, and cross-instance orchestration

> **Status: sketch (not scheduled).** Captures the design direction converged in
> the 2026-06 discussion around RFC
> [#65](https://github.com/MDMTseng/xInsp2/issues/65) (FR-2 / FR-4). Graduates to
> `internals/` when it ships. The read primitives it builds on already shipped.

## The need (two real app shapes)

1. **Fixed-pipeline app, swap configs.** A fixed inspection lane (lens calib →
   pose locate → dimension measure) runs unchanged, but the **inspected object**
   (product) and the **instrument** (station) vary — each needs its own set of
   per-instance configs (sbm templates *incl. images*, measure setup, lens
   calib, taught pose).
2. **Versioned standards.** The same lane sometimes needs different standards
   (e.g. before/after heat-treatment, an experimental tweak), navigated as a
   **branch graph** (the *presentation* of git's commit graph — not git itself).

## Unifying model

The pipeline splits into two layers:

- **Station / pipeline (fixed):** which plugins, instance names, the `inspect.cpp`
  script, and where each node sits in the graph.
- **Config-set (swappable):** per instance, **① def** (small tunables) + **②
  resource folder** (heavy assets as files). A "config-set" is that bundle across
  all instances. `③` runtime working state (accumulators, loaded models) is NOT
  part of a config-set — it rebuilds on switch.

"Switch object" / "checkout version" both reduce to **apply config-set X**: for
each instance, `set_def` (①) + point its resource folder (②); plugins stay warm.
Both app shapes are the same store of config-sets with different organizing
metadata — **labels** (instrument × product → a grid) and an optional **parent**
(fork-from → a branch tree). One flat store, two views; they compose (a product's
"before/after heat-treat" = a product label + a version branch).

## Where it lives — a controller plugin, not the core

Consistent with "lightweight core, power via plugin composition": config-set
storage, the grid/tree, the branch-graph UI (the controller's webview), and
resource-folder assignment are all a **controller plugin's** job. The core adds
**one primitive** — a `host_api` capability letting a plugin address + command
other instances (`get_instance_def` / `set_instance_def` / `exchange_instance` /
`list_instances`), routed through the same `InstanceRegistry` the WS commands and
the script's `xi::use` already use (this is RFC #65 FR-4).

**Crucial scope line:** expose this **on the control plane only** — callable from
a plugin's `exchange()` (operator-driven switching, with the host quiescing
dispatch around the swap), **NOT** from `process()` on the per-frame hot path
(that re-opens the concurrency hazards and is a separate, harder problem). Object
switch / version checkout are inherently control-plane, so they fit cleanly.

The branch-graph is a *presentation*: reuse the existing pipeline-graph rendering
(VS Code extension), with nodes = config-set snapshots and edges = fork-from.
Operations: checkout (apply), fork (branch), compare (run two, diff — the
`compare_variants` idea at config-set granularity), tag/rename. **No merge.**

## Already shipped (the read half)

- `get_instance_def` (WS, mirrors `set_instance_def`) — snapshot an instance's
  full def incl. round-tripped assets like `image_png_b64`. Loop over
  `list_instances` → a whole config-set snapshot. (`reference/ws-protocol.md`.)
- `cmd:run` record injection — feed a record (frame + meta) headlessly without a
  source plugin; useful for the controller/test harness paths.

## Concurrency — mostly already handled

Mid-run config change is **safe by construction for the default (non-reentrant)
plugin**: the per-instance `CallScope` gate (`xi_cabi_adapter.hpp`, cap=1)
serializes `set_def`/`exchange` against an in-flight `process()` — no global
quiesce, the new value applies next frame, the author writes zero protection.
Verified by `backend/tests/test_set_def_race.cpp` (0 torn reads vs ~140k when a
plugin opts into reentrancy). What's NOT yet built: two **opt-in, lock-free**
primitives for the reentrant-high-fps + periodic-peripheral-data cases — a config
**snapshot atomic swap** (RCU-style; process reads a const pointer, set_def swaps
a fresh immutable copy) and a **lock-free inbox queue** (peripheral pushes,
process drains). Reentrant plugins opt into these; non-reentrant never need them.

## Open design questions

1. **Grid vs tree organization** — is the version branch per-product, global with
   product/instrument labels, or grid-then-tree? Decides the config-set key + how
   labels and `parent` are stored.
2. **Version granularity** — is a node a whole-project config-set, or can a single
   instance's config branch independently?
3. **Control-plane only (recommended) vs also hot-path orchestration** — the
   latter (per-frame `A.out → B.in`) is the harder FR-4 half; defer.
4. **Resource-folder reload contract** — when an instance's folder is re-pointed,
   the plugin must re-read its assets: a dedicated `set_folder`/`reload` ABI verb,
   fold it into `set_def`, or an `exchange` convention. Start with the exchange
   convention to validate, promote to a first-class verb once the shape is known.
5. **Operator HMI vs dev tool** — does the branch-graph live only in the VS Code
   extension, or also in the operator HMI (a one-button version/product switch)?

## See also

- RFC [#65](https://github.com/MDMTseng/xInsp2/issues/65) — FR-1..7 (FR-1 shipped
  via `emit_record` + `current_trigger().meta()`; FR-7 mostly via
  `instances.md`/`write-a-plugin.md`; this sketch is FR-2 + FR-4).
- [`production-hmi.md`](./production-hmi.md) — the operator dashboard this feeds.
- `reference/instances.md`, `reference/ws-protocol.md` — the instance + def WS surface.
