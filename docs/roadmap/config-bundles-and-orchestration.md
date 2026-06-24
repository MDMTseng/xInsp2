# Config bundles, object/version switching, and cross-instance orchestration

> **Status: sketch (not scheduled).** Captures the design direction converged in
> the 2026-06 discussions around RFC
> [#65](https://github.com/MDMTseng/xInsp2/issues/65) (FR-2 / FR-4) — incl. the
> 2026-06-24 orchestrator-API + frame-perfect-swap session (verb set, plugin
> tiers, prepare/commit + drain-barrier). Graduates to `internals/` when it ships.
> The read primitives it builds on already shipped.

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

## Prototype landed (task #66, exchange-convention)

The drain-barrier + double-slot shape is now wired and tested, on the
exchange-convention path (open question #4) before any first-class ABI promotion:

- **`cmd:commit_group`** (WS) — the host drain-barrier: quiesce dispatch → drain
  in-flight runs → fire `commit` (or any `cmd`) at every instance in the group in
  one no-process window → resume at prior fps. Reuses the same
  `quiesce_dispatch_for_lifecycle_op_` primitive recompile/rebuild rely on.
  (`reference/ws-protocol.md`.)
- **`plugins/config_swap_probe/`** — reference plugin implementing the double-slot
  `prepare`/`commit` via `exchange()`: `active_`/`staged_` as
  `std::atomic<shared_ptr<const Resource>>`, `process()` reads the live slot
  lock-free, `prepare` stages in the background, `commit` is the atomic swap. Doc-
  by-example of the "push mutable state into the swappable slot → reentrant
  lock-free" rule below.
- **`cmd:get_state`** (WS, task #67) — the host-tracked instance state machine
  (`created`/`active`/`faulted` + `last_error`), driven by the host-visible verbs
  (create_instance / set_instance_def / commit_group). Coarse by design; fine
  staging/ready stays plugin-side (exchange `get_status`). In this prototype the
  host can't see prepare/commit (opaque exchange commands), so they don't move the
  state — task #69 (first-class ABI) lets the host observe them and refine.
- **`test/ws_commit_group.test.mjs`** — proves prepare stages without swapping,
  `commit_group` flips a 2-instance group together, resume-after-commit keeps the
  backend live, a missing target reports a per-target failure, and `get_state`
  tracks created → active.

- **`commit_group` addressing** (task #68) — targets are the deduped union of an
  explicit `instances[]` plus `group` / `plugin` selectors that expand against
  existing instance properties. **Decision:** reuse the dispatch `group` + the
  `plugin` type rather than add a per-instance tag field — zero schema, covers the
  common cohorts ("all of line1", "all binarize"). A dedicated tag is deferred
  until a config-switch cohort must cut ACROSS dispatch groups.
- **First-class `prepare`/`commit` ABI verbs** (task #69) — promoted off the
  exchange convention to optional plugin exports `xi_plugin_prepare(inst,def,folder)`
  + `xi_plugin_commit(inst)` (ABI v7, opt in via `XI_PLUGIN_STAGED`). The host calls
  prepare via the new `prepare_instance` WS command and commit via `commit_group`;
  a plugin without the exports falls back to gated `set_def` / no-op, so simple
  plugins need zero new code. `config_swap_probe` now overrides `prepare()`/`commit()`.
- **Ungated background `prepare`** (task #70) — the adapter calls `prepare()`
  OUTSIDE the CallScope gate (concurrent with `process()`), so a heavy load never
  stalls the pipeline; `commit()` stays gated. The contract — *prepare touches the
  staging slot ONLY* — is the plugin's, signalled by exporting the verb.
  `test_prepare_concurrency` proves it deterministically: prepare completes while a
  `process()` holds the cap=1 slot, commit blocks until the slot frees.

`get_state` above is the host-tracked half; once prepare/commit are first-class the
host *could* observe them to refine `created`/`active` further, but they currently
go through `prepare_instance`/`commit_group` which already mark `active`/`faulted`.
The whole orchestrator config-swap design (tasks #66–#70) is now implemented; what
remains is the higher-level controller-plugin / config-set store / branch-graph UI
(the open questions below), which build ON these primitives.

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
The snapshot-atomic-swap is exactly the T3 "double-resource swap" formalized in
*Orchestrator API* below — see the tier table and the "push mutable state into the
swappable slot" rule for the full model.

## Orchestrator API + frame-perfect config swap (2026-06-24 discussion)

The controller plugin drives switching through a small, uniform verb set. The
write half is what an orchestrator says; the read half is the symmetric
introspection it needs to *not switch blind*:

```
target(name | id | tag)        // address one or a group (tag = "all binarization")
set_folder / get_folder        // bind / read an instance's resource folder
prepare(def, folder)           // optional: stage heavy assets (slow, background)
commit()                       // optional: swap to staged config (fast, atomic)
commit_group([...])            // host-layer barrier over many commits — NOT new ABI
get_instance_def / get_state   // symmetric read + ready/busy/faulted
reload                         // convenience = prepare + commit
```

**The orchestrator always speaks `prepare → commit`; semantics degrade per
plugin.** The ABI additions (`prepare`/`commit`) are pure opt-in — when a plugin
doesn't implement them the host **synthesizes** the default from `set_def`
(`prepare ≡ set_def`, `commit ≡ no-op`), so the simple plugin writes nothing new.

### Plugin lifecycle: host-tracked state, NOT plugin-implemented hooks

We deliberately do **not** introduce a mandatory `on_init/on_start/on_pause/...`
lifecycle interface — that would tax every simple plugin (a `binarize(threshold)`
shouldn't implement six empty callbacks). Instead:

- **Library lifecycle** (`load_plugin`/`unload`/`reload_plugins`) already exists.
- **Instance state** becomes *host-tracked* (`Created / Active / Faulted`), driven
  purely by the orchestrator verbs — zero plugin code. `get_state` reads it; the
  fine staging/ready sub-state (for heavy plugins) is answered plugin-side via an
  `exchange`-style `get_status`, the host doesn't record it.
- **Optional hooks** (`prepare`/`commit`) are the only place a plugin reacts to a
  transition, and only the few that need it implement them.

The mandatory path stays exactly the four points it is today:
`construct → set_def → process* → destruct`.

### Frame-perfect commit = two independent mechanisms

"Frame-perfect" decomposes into two parts that buy *different* things — keep them
distinct:

1. **Double-resource swap (in the plugin).** `prepare` loads into the plugin's own
   **staging slot**; `commit` is a single aligned-pointer `atomic` write. A
   concurrent `process()` reader sees old-XOR-new, both fully-loaded — **this alone
   kills torn reads, lock-free.** The slot lives *inside the plugin*, never in the
   host (host holds no def copies, no slots).
2. **Host commit-hold / drain-barrier.** This does **not** buy no-torn-read (the
   atomic swap already did); it buys (a) **run-level / cross-stage atomicity** — the
   swap lands on a *run boundary*, so a single inspection run never sees old-config
   at stage A and new at stage C across instances — and (b) **safe reclamation** —
   old heavy assets are freed only after drain proves no in-flight run still
   references them (else the pointer swap → UAF on the old slot).

```
commit_group arrives:
  1. close dispatch  (new runs queue, don't enter)
  2. drain           (wait in-flight run count → 0)
  3. each instance commit()   ← provably no process() running → uncontended swap
  4. reopen dispatch (queued runs enter, now on new config)
```

### Why drain-barrier, not per-run version-id

With N continuously-running process lanes there is no global gap to "catch." The
rejected alternative — stamp each run with a config-epoch and have `process()`
resolve def/assets by id — **taxes the hot path forever** (every process pays an
indirection + refcount/GC) to cheapen a **rare** event (operator changes recipe).
Wrong trade. The drain-barrier puts the cost on the rare event: `process()` stays
dumb — "read the one current config" — and switching is a brief, bounded hiccup,
**not a stall**, because `prepare` already loaded the assets in the background, so
the barrier only waits one in-flight run (~ms) then flips cheap pointers. A single
plugin that truly cannot tolerate even one frame of jitter may carry the version-id
scheme *internally* (ungated `commit` path) — its problem, not the core's.

### Gated vs ungated — the one safety pin

This line separates the tiers and is the opt-in responsibility boundary:

- **Default `prepare` (= `set_def`) runs *inside* the CallScope gate.** A simple
  plugin has no staging slot, so "swap directly" touches *active* state → for a
  reentrant plugin that would race `process()`. The cap=1 gate serializes it. Safe.
- **Opt-in `prepare` runs *outside* the gate** (background, concurrent with
  `process()`) — its whole point. The author guarantees it touches **only** the
  staging slot, never active. `commit` then runs gated / post-drain, uncontended.

### Plugin tiers (config-swap axis × concurrency axis)

The categories sit on **two orthogonal axes** — concurrency (non-reentrant vs
reentrant) and commit model (immediate `set_def` vs two-slot) — plus whether the
plugin has mutable config state at all:

> Plugin-author-facing version of this table (with the concrete "what you must do"
> + caveats) lives in [`guides/write-a-plugin.md`](../guides/write-a-plugin.md) →
> "Concurrency & config-change safety". This section is the design rationale.

| Tier | Concurrency | State / assets | Author writes |
|---|---|---|---|
| **T0** | reentrant | none / immutable config | almost nothing — fastest, lock-free for free |
| **T1** | non-reentrant | mutable def | nothing about threads — framework auto-locks |
| **T2** | reentrant | mutable def | a manual lock |
| **T3** | either | two swappable resources | `prepare`/`commit` two-slot |
| T2∩T3 | reentrant | two resources | lock + two-slot (heavy, rare) |

- **T0** breaks the myth that "reentrant ⇒ must lock": reentrant + *stateless/
  immutable-config* needs no lock. (Caveat: `binarize(threshold)` is **not** T0 —
  `threshold` is exactly the racy state, our `test_set_def_race` example.)
- **T3 is orthogonal**, so it crosses with T1/T2; the hardest corner T2∩T3 carries
  both burdens.

### Key design rule — push mutable state into the swappable slot

**Does T3 guarantee T2?** Only the *config* half. The double-slot's atomic swap
gives a reentrant plugin lock-free, consistent config reads — so for the def
itself, no manual lock is needed. But T2's lock exists for **any** shared mutable
state across concurrent `process()` calls (accumulators, caches) — that's
**process-vs-process**, which neither the swap nor the drain touches.

So the canonical lock-free pattern for a high-fps reentrant plugin:

> **Put *all* mutable state into the swappable double-slot and make `process()`
> pure-read against it.** Then T2 and T3 collapse: max throughput (reentrant) +
> frame-perfect switch + zero locks, simultaneously. Only when `process()` must
> mutate shared state per-frame do you fall back to "reentrant ⇒ manual lock."

**Scope line for this whole topic:** it covers **def + resource-folder asset
switching** only. A reentrant plugin's *internal-variable* locking
(process-vs-process) is a **separate concern, the author's** — out of scope here.
The single touch-point is the rule above (a plugin *may* design that lock away by
keeping state in the slot, but that's its choice, not a guarantee of the swap
mechanism).

## Open design questions

1. **Grid vs tree organization** — is the version branch per-product, global with
   product/instrument labels, or grid-then-tree? Decides the config-set key + how
   labels and `parent` are stored.
2. **Version granularity** — is a node a whole-project config-set, or can a single
   instance's config branch independently?
3. **Control-plane only (recommended) vs also hot-path orchestration** — the
   latter (per-frame `A.out → B.in`) is the harder FR-4 half; defer.
4. **`prepare`/`commit` as first-class ABI vs `exchange` convention** — start with
   an `exchange`-convention prototype (`{command: "prepare", ...}` / `"commit"`) to
   validate the staging-slot + drain-barrier shape against a real heavy plugin;
   promote to first-class ABI verbs once the contract (gated/ungated, group barrier,
   reclamation) is proven. The default (`set_def` immediate) needs no new verb.
5. **Operator HMI vs dev tool** — does the branch-graph live only in the VS Code
   extension, or also in the operator HMI (a one-button version/product switch)?

## See also

- RFC [#65](https://github.com/MDMTseng/xInsp2/issues/65) — FR-1..7 (FR-1 shipped
  via `emit_record` + `current_trigger().meta()`; FR-7 mostly via
  `instances.md`/`write-a-plugin.md`; this sketch is FR-2 + FR-4).
- [`production-hmi.md`](./production-hmi.md) — the operator dashboard this feeds.
- `reference/instances.md`, `reference/ws-protocol.md` — the instance + def WS surface.
