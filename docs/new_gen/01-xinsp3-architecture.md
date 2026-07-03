# xInsp3 — Architecture Proposal

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Reference architecture — the ideal form under the philosophy (north star). Not a rewrite commitment; adoption onto xInsp2 is mapped in [`03-adoption-map.md`](./03-adoption-map.md) |
| **Author** | Claude (synthesis of ext_review 01–11 + triage) |
| **Premise** | Same philosophy as xInsp2 — speed-first, minimal core, functionality-as-plugins — rebuilt with the structural lessons the eleven external reviews surfaced |

## Scope

This document answers one question: **if xInsp2 were rebuilt today with the
same philosophy, what should the main architecture look like?** It is not a
feature wishlist and not a migration plan — and it is deliberately written
greenfield, so that the shape of the ideal is not distorted by patch-by-patch
thinking. Which parts get scheduled onto the real system, in what order, is a
separate document: [`03-adoption-map.md`](./03-adoption-map.md). It separates:

1. What xInsp2 **proved** — carried over unchanged.
2. What the reviews showed must be **architectural**, not patched — the six
   structural changes.
3. The resulting **system picture** and repo layout.
4. What **stays out** — the rewrite must not become a feature grab.
5. A **build order** — what to build first and why.

## The unchanged spine

The philosophy is not up for revision. Every review confirmed the bets pay off
where they were actually applied:

- **Speed-first.** Zero-copy images + JSON through refcounted pools, no I/O or
  allocation on the per-frame path, in-process plugins. Review 08 graded the
  steady-state hot path a mature B+/A− — the discipline works.
- **Minimal core.** Dispatch, lifecycle, crash-safety, pools, the ABI. The
  governing test survives verbatim: *"Can this be a plugin? If yes, it must
  be."*
- **Functionality-as-plugins.** Multi-camera sync, record/replay, per-run
  exposure, PLC I/O, line safety — all plugins in v2, all plugins in v3.
- **Frozen ABI + carved interfaces.** Review 06 graded the plugin ABI layer A−:
  frozen layout, build-failing guards, precise load-time refusal, versioned
  `get_interface` handshake. This is the best-engineered boundary in the
  project.
- **One machine, supervisor + compute split.** `fe` respawns `be`; a hard
  plugin crash takes the BE down and that is the accepted trade for zero-copy.
- **The gathering-source trigger model** and **per-group ordered emission**.
- **`expose` as the pattern for core ignorance** — per-run values/images are a
  plugin's job; the core never learns what "viewing" is.

## The diagnosis in one sentence

Across eleven reviews, one pattern accounts for the large majority of serious
findings: **xInsp2 applied world-class contract discipline to exactly one
boundary — the plugin ABI — and left every other boundary (wire protocol,
project file, client codecs, health/state, docs, examples) to manual
synchronization, which drifted.**

Evidence, by boundary:

| Boundary | v2 state | Review |
|---|---|---|
| Plugin ABI | Frozen, guarded, negotiated — **A−** | 06 |
| WS protocol version | `abi:1` never bumped, never enforced, mis-documented | 06, 10 |
| Project file | No schema version; `save_project` silently drops unknown keys | 06 |
| Client codecs | 4–5 hand-rolled envelope parsers, 3 hand-rolled msgpack codecs, zero cross-implementation tests | 10 |
| Event contract | Consumed unevenly — the primary dev client ignores the run-outcome lifecycle entirely | 10 |
| Docs vs code | Protocol reference points at files that don't exist; contracts promised but unimplemented | 06, 07, 09, 10 |
| Examples/templates | 32 scripts / 181 calls against a removed API; three templates teach three styles | 11 |
| Test gates | No CI; fixtures pin unimplemented wire shapes; fuzz wired to nothing | 07 |
| Health/state | No canonical contract; four reviews independently asked for one | 01, 02, 03, 05 (Bucket E) |

None of these are philosophy failures. They are all the same failure: a
representation kept in sync by hand. The v3 architecture exists to make that
class of failure structurally impossible, the same way the ABI freeze made
casual layout breaks impossible.

## The six structural changes

### 1. Contract-first: one schema source, everything else generated

The single biggest change. A top-level `contract/` package becomes the
machine-readable source of truth for every cross-boundary representation:

- the WS envelope and every message (`cmd`/`rsp`/`event`/`log`),
- the run-result / run-outcome schema,
- the binary preview frame format (v2's `XEX1`),
- the project / instance file formats,
- the health/state contract (change #2).

From it, the build **generates**: C++ serializers for the core, a TypeScript
client-core (consumed by the extension, HMI, and ui-components), a Python
client-core (the v3 `xinsp_py`), the wire fixtures, and the protocol reference
skeleton. Hand-written protocol documentation describes *semantics*; shapes are
generated and therefore cannot drift.

What this kills, by construction: the five independent envelope parsers and
three msgpack codecs (10), the decorative protocol version (06), fixtures that
pin shapes the backend doesn't emit (07), and the phantom-file protocol docs
(10). The plugin ABI stays hand-carved C — that boundary already has its
discipline and codegen would add nothing.

**Cost accepted:** a codegen step in the build. It is the same trade ADR-001
made — pay a small fixed process cost to delete an entire class of drift.

### 2. The health/state contract is core, and is built first

Bucket E of the triage records that a canonical health/state contract was the
one recurring ask (four reviews) that fails the plugin test — it is genuinely
core-shaped — and that "if a coherence pass is ever undertaken, this is the
first thing to build." A rewrite **is** that coherence pass.

v3 core owns one small, versioned contract:

- a single top-level state machine: `boot → project_loaded → running →
  degraded → draining → fault`, with defined transitions;
- per-component health (script, each instance, each dispatch group, each
  connected source) as `ok / degraded / failed + reason_code`;
- the run-outcome identity slice from day 1: `station_id`, `boot_id`,
  `inspection_id`, `trigger_id`, script generation, `schema` version on every
  result (v2 retrofitted this additively; v3 starts with it).

The FE supervisor, HMI, extension, PLC plugin, and headless runner all consume
the *same* contract instead of each inferring liveness from side channels.
This is deliberately small — a state enum, a component map, and reason codes —
not an observability platform. Metrics, history, dashboards remain plugins.

### 3. Version identity on every boundary, enforced at every reader

The ABI layer's discipline, extended to the three boundaries review 06 found
naked:

- **Project files carry `schema` + `version`**, and the persistence model is
  **read–modify–write over the full document**: a save preserves every key the
  writer doesn't own. v2's sharpest data-loss risk (`save_project` rebuilding
  `project.json` from two keys) becomes unrepresentable — there is no code
  path that reconstructs a file from partial knowledge.
- **The protocol version is real**: bumped on every breaking change (enforced
  by a gate that diffs the generated schema against the released one), sent in
  `hello`, and **checked by the generated client-core** — so every client
  enforces it for free, fixing the client half of the drift (10).
- **Skew is a first-class UX state**: extension/HMI vs backend version
  mismatch renders as an explicit "incompatible, here's why" state, not a log
  line.

### 4. Total failure boundary by construction, not by discipline

Review 08's residual risks and 09's headline finding share a shape: the
per-frame path is guarded by carefully maintained RAII discipline, but
anything *outside* that path (command handlers, diagnostics walks, foreign
threads) relies on each author remembering. v3 makes the guards structural:

- **One dispatch shell.** Every command handler runs inside a framework-owned
  `try`/`catch` that turns any escape into a structured `rsp` error. The
  documented contract ("handler exception → `ok:false`") is implemented once,
  in one place, and cannot be forgotten per-handler (09).
- **Blessed concurrency is the only concurrency.** The SDK's thread primitives
  (`xi::parallel_for`, `xi::async`, `spawn_worker`) install the SEH
  translator, owner tagging, and crash breadcrumbs. Raw `#pragma omp parallel`
  in scripts is rejected at compile time (the JIT compiles with OpenMP off;
  the pragma without the wrapper is a doc'd, detected error), so a fault on a
  foreign thread can no longer kill the backend untranslated (08).
- **Diagnostics are as safe as the hot path.** Pool stats and other management
  walks use the same epoch/refcount protocol as the per-frame path — designed
  in, so a UI polling stats can never race a concurrent release (08).
- **Malformed input never goes silent**: an unparseable envelope gets a
  correlated error response if any id is recoverable, and always counts on a
  visible reject counter (09).

### 5. Gates are day-1 infrastructure; exemplars are tests

Review 07's verdict was that green is "a local claim, not an enforced
invariant"; review 11 found the teaching surface compiling against a dead API.
Both are solved by the same rule: **if it is shipped as truth, it is built in
CI.**

- CI exists from the first commit: ctest, the QA driver, protocol round-trip
  tests against a real spawned backend, and the fuzz smoke — all
  build-breaking.
- **Every example, template, and doc snippet compiles in the gate.** The
  examples tree is a test suite that happens to be readable. 181 calls to a
  removed API (11) cannot happen; the scaffold cannot rot (02 P0).
- **One template spine.** A single plugin base class with layered opt-ins
  replaces v2's three templates teaching three styles. `easy/medium/expert`
  become *the same skeleton* with more layers uncommented, not different
  architectures (11).
- Fixtures are generated from `contract/` (change #1), so a fixture that pins
  an unimplemented shape (07) is impossible — the shape *is* the
  implementation's source.

### 6. The core is composed, not accreted

v2's `service_main.cpp` became "the big one" — command handling, dispatch
pools, script lifecycle, and the crash filter in one file. Splitting it was
originally triaged as churn *for v2* (Bucket D). *(Update 2026-07-02: v2 has
since adopted the TU-level split — `service_main.cpp` → `service_cmd_*` /
`service_dispatch` / `service_inspect` / `service_toolchain` /
`service_result` / `service_sinks` over a `service_internal.hpp` shared
surface, plus `xi_plugin_manager.hpp` → `xi_pm_*` sub-headers — along
essentially the seams proposed below. The remaining v3-only content of this
change is the ownership-object model, not the file layout.)* v3's core is a
small set of subsystem owners behind the same minimal surface:

```
core/
  dispatch/        trigger bus, groups, ordered emit
  pools/           image pool, doc registry (epoch-safe stats built in)
  lifecycle/       project open/save (full-document), script JIT, hot-reload
  faults/          SEH boundary, breadcrumbs, dumps, quarantine policy
  health/          the state contract (change #2)
  wire/            WS server + generated codecs + the one dispatch shell
  abi/             the frozen C ABI + carved interfaces (unchanged design)
```

Two smaller correctness upgrades ride along, both flagged by 08:

- **Post-fault quarantine policy**: an instance whose `process()` faulted
  mid-mutation is marked `degraded` in the health contract and its reuse
  policy (reuse / re-init / refuse) is explicit per-plugin, not implicit.
- **Crash breadcrumbs sized for long-run**: per-thread slots recycle with
  thread exit instead of leaking toward a racy shared slot.

## The system picture

Runtime topology is **unchanged** — it was never the problem:

```
             ┌────────────────────── one machine ──────────────────────┐
             │ xinsp-fe ──spawns──► xinsp-be ────WS───► clients        │
             │ (supervisor)         (compute core,      (extension,    │
             │  respawn, crash      in-process plugins,  HMI, python)  │
             │  history)            health contract)                   │
             └─────────────────────────────────────────────────────────┘
```

What changes is what sits **between** the parts:

```
contract/  ──generates──►  core/wire codecs
           ──generates──►  clients/client-core-ts   ◄── extension, HMI, ui-components
           ──generates──►  clients/client-core-py   ◄── xinsp_py, headless tooling
           ──generates──►  fixtures + protocol reference skeleton
```

Repo layout:

```
contract/          schemas: protocol, run-outcome, project file, health  ← NEW, the root of truth
core/              the BE, composed as §6                                 (v2: backend/src + include)
supervisor/        the FE                                                 (unchanged role)
abi/               frozen C ABI headers + carved interfaces + guards      (v2 design, kept)
sdk/               ONE template spine + examples (compiled in CI)
clients/
  client-core-ts/  generated transport+codecs+abi-check (shared)          ← NEW
  client-core-py/  generated                                              ← NEW
  extension/       consumes client-core-ts; consumes the FULL event contract
  hmi/             consumes client-core-ts (no hand-rolled socket)
plugins/           shipped plugins — each is also a conformance exemplar
gates/             CI definitions; examples-compile, schema-diff, fuzz smoke, protocol round-trip
docs/              semantics by hand; shapes generated
```

## What stays out

The triage's Buckets C and D survive the rewrite intact. A rebuild is the most
dangerous moment for scope creep, so this is explicit:

- **Still plugins, never core:** evidence journal, integrity signing,
  retention, recipe catalog/approval/rollback, MES delivery, physical-part
  identity, history/analytics, viewers.
- **Still deferred until real demand:** operator-mode tiers
  (dev/commission/prod), approval/audit machinery, optimistic concurrency,
  determinism levels D0–D3, a machine-wide concurrency budget, soak
  infrastructure.
- **Not adopted:** multi-process plugin isolation (kills zero-copy — the v2
  trade stands), a scripting language other than C++, protocol transports
  beyond WS.

The two 05 items triage flagged as "most defensible if a user hits them" — a
bounded `xi::async` executor and moving JPEG/viewer encode off the ordered
result path — are **design-accommodated** (the dispatch and emit interfaces
leave room) but not built.

### Known hard limits — explicit non-goals, with named escape hatches

*(added 2026-07-03)* Recorded so each limit reads as a deliberate decision,
not an oversight. Format per row: the limit, the blessed workaround, and the
concrete trigger that would reopen the decision.

| Limit (deliberate) | Blessed escape hatch | Revisit when |
|---|---|---|
| **No sub-frame feedback loops** — a result cannot steer the *same* frame's capture; dispatch is one-way per event | Frame-level feedback IS supported: the script drives the source's own pack door (`xi::use(source).process(pack)`) so frame N's result steers frame N+1 | A real control loop that measurably cannot tolerate one frame of latency |
| **No sub-chunk streaming** — a pack is sealed whole; consumers never see partial entries | Split the payload into multiple packs under the chunking convention — doc 18 (in flight); correlate with `$seq`/entry keys | A payload that cannot fit memory, or a latency budget that demands pipelined partial delivery, on a real line |
| **No cross-instance GPU handoff** — device memory never crosses the ABI; pack entries are host memory | GPU-island: keep the whole GPU pipeline inside one instance; exchange via the resource-handle convention (doc 14 appendix) | Two shipped plugins that must share device buffers AND measurement showing the host round-trip copy dominates |
| **No preemptive scheduling** — an in-flight inspect runs to completion; no priorities, no cancellation mid-frame | Partition by dispatch groups + bound per-frame work (`xi::parallel_for`/`xi::async`); size the worst case to the takt | A mixed-criticality deployment where group partitioning demonstrably cannot meet a hard takt |
| **No in-process trust boundary** — plugins are trusted native code; SEH isolation is crash *safety*, not a sandbox | Trusted-load policy (review what you deploy); genuinely untrusted work runs out-of-process behind a proxy plugin you own | An actual third-party/untrusted plugin requirement (note: multi-process isolation was already rejected — it kills zero-copy; that trade stands) |

## Build order

The order is chosen so every later layer is born inside the discipline, never
retrofitted into it:

1. **`contract/` + codegen + CI skeleton.** Schemas for envelope, run-outcome,
   health, project file; generated C++/TS/Py codecs; the schema-diff gate.
   Nothing else exists yet, so nothing can drift — ever.
2. **Core skeleton: pools + ABI + faults.** Port the v2 designs that earned
   A−/B+ (image pool with epoch-safe stats, doc registry, frozen ABI +
   guards, SEH boundary + breadcrumbs v2-lessons applied).
3. **Health contract + dispatch.** The state machine first (change #2), then
   trigger bus + groups + ordered emit reporting into it.
4. **Wire layer.** WS server (port v2's hardened transport nearly verbatim —
   09 graded it A−) + the one dispatch shell + generated codecs.
5. **Script lifecycle.** JIT, hot-reload (`module_lifetime` design carries
   over), full-document project persistence.
6. **SDK + first plugins.** The single template spine; port `expose`,
   `mock_camera` (rewritten to the blessed patterns — it is the most-copied
   file in the project), one detector. Examples tree compiles in CI from the
   first example.
7. **Clients.** Extension and HMI on client-core-ts — the extension consumes
   the full event contract from day 1 (10's headline gap).
8. **Supervisor + runner.** FE respawn logic and headless runner over the
   same health contract.

## Decision checklist

Decisions the maintainer must make before any of this is real:

- [x] **Rewrite vs. incremental?** Resolved by reframing: this document is
      the north star, not a rewrite plan. Changes #1–#5 are adoptable onto
      xInsp2 incrementally; #6 is greenfield-only. See
      [`03-adoption-map.md`](./03-adoption-map.md). (A clean-break vs. shim
      question would only revive if a greenfield core is ever undertaken;
      recommendation on record: clean break with a porting guide.)
- [ ] **Schema language for `contract/`** (JSON Schema / a typed IDL /
      hand-rolled). Criterion: must generate readable C++ that respects the
      zero-copy pools, not just idiomatic TS/Py.
- [ ] **Scope of generated docs** — full protocol reference vs. shapes-only
      skeleton with hand-written semantics (recommended).
- [ ] **Whether `fe/include` (file-based status) survives** or is replaced by
      a health-contract consumer.

## Final judgment

xInsp2's philosophy survived contact with eleven adversarial reviews; its
*manual synchronization* did not. The v3 architecture is therefore not a new
idea but a generalization of the project's own best idea: **what ADR-001 did
for the plugin ABI — freeze the shape, guard it in the build, negotiate it at
load — applied to every other boundary in the system.** The hot path, the
supervisor topology, the plugin model, and the trigger design port forward
nearly unchanged, because the reviews validated them. What changes is that in
v3, a representation that two parties share is either generated from one
source or guarded by a gate — never kept true by memory.
