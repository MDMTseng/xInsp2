# Polaris2 — Synthesis of Four Independent Next-Gen Visions

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Inputs** | [`vision-a`](./vision-a.md) · [`vision-b`](./vision-b.md) · [`vision-c`](./vision-c.md) · [`vision-d`](./vision-d.md) — four architects, same brief, same philosophy, NO access to docs/new_gen or docs/ext_review (deliberately unanchored) |
| **Compared against** | [`../01-xinsp3-architecture.md`](../01-xinsp3-architecture.md) (our north star) and [`../02-plugin-data-contract.md`](../02-plugin-data-contract.md) |
| **Method** | Each vision independently: (1) checked the polaris_master code, (2) designed an ideal next generation. Agreement across blind samples is evidence; so is a shared blind spot in OUR plan |

## 1. Where all four converge (and what that proves)

**1a. "One fact, one owner, derived projections, gated" is the constitution.**
All four independently landed on the same central law, and three of them
independently named the SAME file as its proof: `xi_health.hpp`, which stores
only the facts no other owner holds and derives everything else on read.
B: "the residual risk is ONE shape wearing many costumes"; C: "the recurring
bug is ONE bug: a fact represented in two places"; D: "own once, project
everywhere, gate the projection." This is our north star's change #1/#3
(contract-first + version identity) restated from scratch — **independent
convergence is the strongest validation the plan has received.** They push it
one notch further than we did: generate the ABI surface, the clients, and the
version numbers from one declaration, so there is *nothing left* to hand-sync.

**1b. The same primitives are sacred.** All four keep verbatim: the ImagePool
(slot⊕generation ABA defense, Treiber free-list, WalkGuard deferred
reclamation), the C ABI + SEH crash boundary, the trigger-as-pure-funnel /
gathering-source model, the InflightRuns Dekker teardown, and the
verdict-honesty band. Nobody proposed multi-process isolation; nobody
proposed replacing C++ scripts. The philosophy's core bets survived four
blind re-derivations.

**1c. The client fleet must collapse into generated per-language cores** —
unanimous, and identical to our adoption item 8.

**1d. The check passes converged on the same defects** (see §4) — including
one that all four found: the contract gate's silent skip. Four blind reviewers
finding the same bug is how you know the bug class is structural, not
incidental.

## 2. The big divergence from OUR plan: the metadata plane

This is the finding that matters most, because it is where four blind samples
disagree with our documented decision.

**Our 02 decision:** keep the schemaless mutable JSON `Record` as the only
currency across the per-frame ABI; typed accessors are a view layer.

**All four visions, in different words, said: delete it from the per-frame
boundary.**

- **A**: one per-frame **arena** owning pixels + JSON + verdict; borrowed
  const views in, outputs written back, freed in one shot at frame end.
- **B**: keep image zero-copy verbatim; replace the shared-mutable
  yyjson-DOM-by-pointer channel with a **flat immutable typed value buffer**.
- **C**: **typed flat POD record**, copied on the rare cross-instance hop ("a
  memcpy beats one contended atomic CAS"); JSON only at the wire edge; typed
  schema-declared record as the primitive, schemaless as the escape hatch.
- **D**: typed **arena value-type `xi::Frame`**; images stay pool handles;
  JSON only at wire/config/disk.

Their shared argument: the metadata channel is a **kilobyte-scale** payload
riding next to megapixel images. Zero-copy *sharing* of that channel is what
forces the two refcount registries, COW/freeze, the share_out/adopt_shared
reserved-ref handshake, the cross-CRT free dance — and its known costs are
not hypothetical: the **deliberate per-crash doc leak** (`xi_use.hpp:558-579`,
leak-over-UAF on every caught plugin crash) and the hardest ownership
reasoning in the tree. Buying microseconds on the small channel with the
system's most dangerous machinery is a bad trade; **value/immutable semantics
per frame dissolve the machinery and the leak class at once.**

**Reconciliation with 02 (important — the two are less opposed than they
look).** 02 rejected *typed structs across the ABI* for two reasons: (r1)
raw struct layout + JIT hot-reload skew = silent memory corruption; (r2)
generic plugins (record_save / expose / data_output) need self-describing
data. The visions' proposals differ from what 02 rejected:

- A/B/D propose **value/arena semantics with a defined encoding**, not shared
  raw C++ struct layout — and D generates the encoding + accessors from one
  declaration (`operator.toml`), so skew is a **gated, versioned condition**,
  not corruption. (C's bare "POD record" IS layout-sensitive and would need
  the same generated-encoding discipline to satisfy r1.)
- r2 (self-description for generic plugins) is preserved if the flat encoding
  is **tagged** (field-id + type per entry, msgpack/flatbuffer-style), which
  B's "immutable value buffer" and D's arena both admit. Generic plugins keep
  introspecting; they lose nothing but the shared-mutability they never
  needed.

**Synthesized position for the north star (amendment candidate):** keep 02's
*contract* layer exactly as adopted (keys, builders/extractors, schema
stamp, fail-loud) — every vision independently reinvented it (D wants it
generated from `operator.toml`, which IS our 02 stage 2). But amend the
*runtime representation* goal: the per-frame metadata channel becomes an
**immutable, tagged, arena-allocated value** (write-once by the producer,
borrowed const views across the ABI, one free at frame end; images stay pool
handles). Zero-copy sharing — with its refcounts, COW, and crash-leak — is
retired from the small channel and kept only where it pays: pixels.
This supersedes nothing in xInsp2 today; it is the v3 data-plane target.
*(Refined and adopted 2026-07-02 as [`../07-uniform-keyed-buffer-plane.md`](../07-uniform-keyed-buffer-plane.md), which erases the remaining image/meta split: one keyed container of typed binary buffers, msgpack as the default metadata encoding.)*

## 3. The second divergence: kill the monolith struct (B + C)

B and C independently concluded the same thing about the ABI: v11 didn't
retire the monolithic `xi_host_api` struct — it **doubled** it. The struct
fields AND the carved `get_interface` doors coexist, kept in lockstep by a
runtime assert (`door_matches_fields`) and a forwarder-slot bridge — i.e., a
hand-synced second representation, the exact disease the constitution bans.
Their fix: **ship only `get_interface(id, version)` from line one**; retiring
a capability becomes "return null to a query," not a versioned layout break.

Our 01 said "abi/: frozen C ABI + carved interfaces (unchanged design)" —
this is a genuine amendment: **in the greenfield core, there is no struct;
the door is the only access path.** (On v2 it stays as-is: removing the
struct is a breaking ABI event with no v2 payoff.)

## 4. Check findings — triage

Converged findings, fixed on polaris_master immediately (commit `2c3bb4b`):

| Finding | Found by | Status |
|---|---|---|
| Contract gate silently no-ops (jsonschema absent → validate.py exit 0; 4 fixtures landed unmapped under a green gate; CI never provisions the dep) | A, B, C, D | **FIXED** — health schemas added, fixtures mapped, baseline refreshed; skip now marked NOT-A-PASS and hard-fails under `XINSP2_REQUIRE_SCHEMA_GATE=1` (set by gate.py); ci.yml installs jsonschema |
| README §9 claims runner verdict capture "not yet implemented" — shipped long ago; README self-contradicts | A, B, C, D | **FIXED** — rewritten to the truth incl. the exit-code caveat |
| `synced_stereo` `fps_` data race (control-thread write vs worker read) | B | **FIXED** — atomic, matching mock_camera |

Real, deferred (design-rooted or needing their own pass):

| Finding | Found by | Disposition |
|---|---|---|
| Deliberate per-crash doc leak (`xi_use.hpp:558-579`: reserved share_out ref never released on a `-2` crashed call; unbounded under a plugin faulting every N frames) | B, D | Dissolves under the §2 data-plane amendment; until then: a bounded quarantine already limits repeated faulting (on_fault reinit/refuse); flag in docs |
| STACK_OVERFLOW translated + worker kept alive without `_resetstkoflw` → later frames on a holed guard page | A | Small, real: schedule a fix (call `_resetstkoflw` on the catch path or hard-refuse the lane) |
| Shipped sources use raw `std::thread` + free `emit_record` (mock_camera.cpp:155, synced_stereo.cpp:74) against the SDK's own spawn_worker guidance — fault outside the SEH translator kills the BE | D (B adjacent) | Schedule: port both to `spawn_worker` (exemplar honesty — same class as review 11) |
| `blob_analysis` typed `Output` can't read the contour its own plugin writes — the flagship `_io.h` is a leaky veneer; only 2/9 plugins follow `_keys/_io` | D | Schedule: complete the accessor + track coverage; strengthens the case for 02 stage 2 (generate, don't hand-write) |
| No test validates LIVE backend bytes against the schemas (schemas + fixtures are both hand-authored mirrors; codegen runs schema→types, never source→schema) | A | Schedule: a wire-capture conformance test (spawn backend, capture real messages, validate against contract/) — the missing third leg of the contract gates |
| Minor: `lane_for_` falls back to `.front()` despite its own "never silently the front" comment; `xi_protocol.hpp:5` points at nonexistent `protocol/messages.md`; `data_output.cpp:51-54` save is a stub | D | Batch as small doc/code truth fixes |

## 5. What this round changes in our plan

1. **Validates** the six structural changes of 01 — four blind re-derivations
   converged on the same constitution and the same sacred primitives.
2. **Amends the v3 data plane** (§2): metadata = immutable tagged arena value;
   zero-copy sharing retired to pixels-only. 02's contract layer unchanged;
   02's "Record stays the ABI currency" holds for v2 but the v3 target
   representation moves.
3. **Amends the v3 ABI** (§3): greenfield ships pure `get_interface`, no
   monolith struct.
4. **Adds follow-up work** (§4 deferred table): stack-overflow reset, raw-thread
   exemplar ports, blob Output completion, live-wire conformance test, minor
   truth fixes.
5. **Confirms 02 stage 2 direction** (D's `operator.toml` = our "published
   schema, generated views") — when a second consumer appears, generate.
