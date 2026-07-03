# polaris2_main — Implementation Plan

> **Naming (post-pilot):** the wave-1/2 container this plan calls **Frame**
> (`xi_frame.hpp`, the `xi.frame@1` door, `t.frame()`) was renamed **Pack**
> (`xi::Pack`, `xi.pack@1`, `t.pack()`) after the pilots — zero image
> connotation. The plan/verdict text below is kept verbatim as the historical
> record; see [`07`](./07-uniform-keyed-buffer-plane.md) for the current names.

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Branch model** | `polaris2_main` branches from `polaris_master`; task branches from `polaris2_main`; merges ONLY into `polaris2_main` (master untouched, same rule as Polaris 1) |
| **Goal** | Turn the polaris2 decisions ([`07`](./07-uniform-keyed-buffer-plane.md) data plane, [`polaris2/00-synthesis.md`](./polaris2/00-synthesis.md) amendments + deferred findings) into working code and measured evidence — WITHOUT a big-bang rewrite |
| **Strategy** | Strangler, round two: build the v3 data plane's foundations beside the v2 Record, pilot them through ONE carved interface on real plugins, and let benchmarks + the pilot decide migration scope. Every wave ends gate-green |

## The one strategic call

07's frame plane is "not a v2 retrofit" as a *replacement* — but every
foundation it needs is **additive**: a codec header, a container header, a
carved `get_interface` door, generated accessors. v2's Record keeps running
throughout; the new plane earns its way in with measurements, exactly how
Polaris 1 earned each merge with gates. The decision to *migrate* is taken
at the wave-2 exit, on evidence, not now.

## Wave 0 — hygiene: the polaris2 deferred findings (parallel, small)

The synthesis §4 deferred table, executed as Polaris-1-style slot tasks:

| Task | Content | Source |
|---|---|---|
| 0a | `_resetstkoflw` on the caught-STACK_OVERFLOW path (or hard-refuse the lane) + a test | vision A |
| 0b | Port `mock_camera` + `synced_stereo` worker threads to the blessed `spawn_worker`; exemplar honesty | vision D |
| 0c | Complete `blob_analysis` `_io.h` (contour accessor its own .cpp writes); extend `_keys/_io` coverage beyond 2/9 plugins (at least the source/detector pairs) | vision D |
| 0d | **Live-wire conformance test**: spawn the real backend, capture actual WS messages, validate the bytes against contract/schemas — the missing third leg (schemas and fixtures are both hand-authored today) | vision A |
| 0e | Truth batch: `lane_for_` front-fallback comment, `xi_protocol.hpp` phantom messages.md pointer, `data_output` save stub honesty | vision D |

0d is the one with architectural weight: it closes the loop the contract
gates still leave open, and it will guard every later wire change this plan
makes.

## Wave 1 — foundations (parallel, additive, no v2 behavior change)

| Task | Content | Exit criterion |
|---|---|---|
| 1a **canonical codec** | `xi_mp.hpp` (or small TU): encode/decode of the canonical max-width msgpack profile — fixed-width numerics, wide container markers, bounded-depth validating reader, NO ext acceptance by default. Golden fixtures (extend protocol/fixtures/binary discipline); a libFuzzer target behind XINSP2_FUZZ | Goldens green in C++/TS/Py readers; fuzz smoke wired into gate |
| 1b **Frame container** | `xi_frame.hpp`: arena allocator + keyed entry table + `seal()` + per-frame key→offset index + typed accessor templates; pool-handle ext minted only by the allocator. Unit tests incl. the lifecycle story (produce→seal→borrow→drop) and crash-drop semantics | Full unit coverage; API review against 07's D1–D3 |
| 1c **ingress canonicalizer** | `xi::canonicalize(span) → Frame entry` implementing 07's three layers (structural / profile / semantic-when-tagged); the ONLY public constructor from foreign bytes; hostile-input tests (nesting bombs, length lies, forged ext) | Rejects every hostile fixture; fuzz target |
| 1d **the measurement** | Extend `bench_hotpath` with a Frame lane: emit→inspect→result carrying Frame vs Record — the perf claims 07 makes (arena bump vs DOM alloc, memcpy-on-hop vs refcount CAS, O(1) sealed reads) measured on the same harness, p50/p95/p99 | **GO/NO-GO evidence** for wave 2 |
| 1e **BufferPool audit** | Determine whether ImagePool already serves as the typeless large-buffer pool (it is nearly one) or needs a thin generalization; NO rewrite — the pool is sacred per all four visions | A one-page verdict + whatever thin shim is needed |

**Wave-1 exit gate:** 1d's numbers. If Frame does not beat-or-match Record on
the hot path, 07 gets re-litigated with data — that is a success of the plan,
not a failure.

## Wave-1 exit gate — VERDICT (2026-07-02, dev-box numbers, medians of 5+)

**GO on the concept, with one condition.** bench_frame (perf_frame) measured
the two planes on the identical span and workload:

- The representation doc 07 specifies (canonical fixed-width msgpack +
  precomputed-offset reads + memcpy hop) **beats Record everywhere**:
  metadata micro ~752 vs ~923 ns/op (~18%), and at parallel=8 the dispatch
  p99 tightens from ~33µs to ~22µs (the contended-DocRegistry-CAS claim,
  confirmed under concurrency).
- The SHIPPED xi_frame.hpp container currently LOSES the isolated micro
  (~1145 ns): seal() builds a runtime hash index and get_* does string-hash
  lookups; fresh heap arena chunk + per-key intern per frame. Implementation,
  not concept.

**Condition for wave 2:** the pilot's container work realizes the
offset-accessor read path (contract-declared key order → precomputed
offsets; arena chunk reuse) before the pilot's numbers are read. Dev-box
caveat applies; the perf runner captures the real baseline later.

*Condition MET (same day): the frame-offset-reads branch shipped
TypedFrame/FrameSchema (compile-time keyset slots, per-thread arena
recycling, no intern on declared keys). Re-measured micro: TypedFrame 522
ns/op vs Record 924 (43% win, also beating the raw mp-plane hop at 761);
the dynamic string-keyed fallback 793 also now beats Record. Keyset drift
is a compile error. Wave 2 is clear to proceed.*

## Wave 2 — the carved pilot (sequential-ish; the strangler bite)

1. **`xi.frame@1` carved interface**: a new `get_interface` door exposing
   frame-in/frame-out process + emit. Additive; ABI v11 untouched; Record
   path untouched. (This is also the dry run for the pure-door ABI amendment
   — the door is designed as if the struct didn't exist.)
2. **Pilot pair**: `mock_camera` emits Frame; `blob_analysis` consumes via
   its (wave-3-generated or hand-written-to-convention) accessors. The
   dispatch/inspect path carries Frame alongside Record transitionally.
3. **Generic proof**: `expose` walks Frame entries without producer knowledge
   (the r2 constraint made real); XEX1-v2 = canonical frame dump — the
   memory≈wire≈disk claim demonstrated end-to-end, guarded by 0d's live
   conformance test.
4. **Script surface**: minimal `t.frame()` accessors so an example script
   runs the pilot pair end-to-end; one QA example added to the gate.

**Wave-2 progress (2026-07-02/03):** steps 1–2 SHIPPED (frame-door branch):
the `xi.frame@1` host door (opaque handle + ~23 C accessors, FrameRegistry,
fail-closed getters) and its plugin-side `xi_plugin_get_interface` mirror —
the pure-door dry run worked with zero friction. Pilots are BILINGUAL
(Record path untouched; frame surface opt-in; no silent conversion in v0).
Dual-carry rides the same lane/EmitGate machinery. Wave-3 seed also landed:
decl → generated `_keys/_schema/_io` headers with a byte-level +
compile-level equivalence gate (codegen_equiv, negative-tested); swap-in
deferred to wave-2 exit per the codegen README.

**Pure-door finding for the synthesis §3 record:** an opaque-handle door
necessarily gives up TypedFrame's compile-time offset reads AT THE BOUNDARY
— cross-DLL consumers read by key string through host accessors; the 522ns
slot read is a same-DLL property. The greenfield §3 core therefore keeps
TypedFrame in-process and defines the cross-door contract as opaque handle +
accessors, with cross-door offset reads arriving via codegen handing the
canonical msgpack plane over as a span the consumer indexes with its own
schema offsets.

**Wave-2 exit status (2026-07-03): steps 3–4 shipped** — expose walks
Frames generically, XEX1-v2 opt-in with the memory≈wire≈disk identity test
passing, `t.frame()` + qa_frame_pilot in the gate, codegen swap-in executed
(ratchet empty; io gap recorded). Full verify: backend 73/73, plugins 10/10,
QA 22/0. The written decision: [`10-pack-migration-scope.md`](./10-pack-migration-scope.md) (DRAFT).

**Wave-2 exit gate:** pilot runs under gate.py fully green + a written
migration-scope decision (which plugins/paths move when; what Record's
deprecation horizon is; what rides the eventual cutover train).

## Wave 3 — contract codegen (02 stage 2, seeded by the pilot)

- One declaration per pilot plugin (the schema file style already in
  contract/) → generated `_keys.h`/`_io.h`, TS + Py types, and docs — the
  generator extends contract/codegen/gen_types.py. Hand-written accessor
  files for the pilot pair are replaced by generated ones with zero
  call-site changes (02's stage-2 promise, proven).
- The `_keys/_io` coverage gate: a ctest asserting every plugin with a
  declaration has current generated artifacts (drift = red).

## Sequencing & governance

- **Wave 0 ∥ Wave 1** — fully parallel (7–9 slot tasks, same worktree-pool
  discipline; xr-10 becomes the polaris2_main integration/verify slot,
  xr-09 stays on polaris_master).
- Wave 2 starts only after 1a+1b+1d land (needs codec, container, evidence).
- Wave 3 after the pilot stabilizes the accessor convention.
- Every merge: full gate.py on polaris2_main. Breaking-wire anything
  (XEX1-v2 as default, abi bump) stays OUT — staged notes only, for the
  app-team cutover train.
- Out of scope for this line: the greenfield pure-door core (the door pilot
  informs it; building it is a separate decision), multi-process isolation
  (still rejected), any master merge.

## First dispatch (when execution starts)

Batch 1 (8 agents): 0a, 0b, 0c, 0d, 0e, 1a, 1b, 1c — 1d follows 1a/1b
(needs both), 1e can ride with 1b's author. Wave-2 tasks are cut only after
the wave-1 exit gate is read.
