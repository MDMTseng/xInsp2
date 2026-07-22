# xInsp3 → xInsp2 — Adoption Map

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Planning aid — maps the ideal ([`01`](./01-xinsp3-architecture.md), [`02`](./02-plugin-data-contract.md)) onto xInsp2 as schedulable increments |
| **Rule** | The ideal is the north star; nothing is adopted because the ideal says so — each increment must pay for itself on xInsp2's own terms (the triage lens still governs) |

## Classification

Every element of the ideal architecture falls into one of four classes:

- **NOW** — non-breaking, independent, cheap; schedulable immediately on
  master (Bucket-A-like).
- **CARVE** — additive new surface (a carved interface, new messages, new
  headers); no wire break, but a real design, so deliberate (Bucket-B-like).
- **CUTOVER** — breaking; rides a coordinated cutover train with the app
  team, alongside the already-staged `integration/extreview-breaking` set.
- **GREENFIELD-ONLY** — valuable only in a from-scratch build; do **not**
  retrofit. Kept on record so it isn't re-litigated.

**Standing constraint:** the already-staged `integration/extreview-breaking`
cutover (wire renames, `XI_SYS_CRASHED`, HMI throughput semantics, partial
status) is not blocked by anything here — it lands first, on its own
schedule.

## The map

| # | Ideal element (01/02) | v2 increment | Class | Depends on | Review basis |
|---|---|---|---|---|---|
| 1 | One dispatch shell (change #4) | Top-level `try`/`catch` around `handle_command`; structured `rsp` error; malformed-envelope correlated error + reject counter | **NOW** | — | 09 headline |
| 2 | Diagnostics as safe as hot path (change #4) | Fix `ImagePool::stats()` / `stats_by_owner()` UAF; WS `client_` teardown under lock | **NOW** | — | 08 top findings |
| 3 | Gates as day-1 infra (change #5) | Stand up CI: ctest + run_qa + fixture round-trip + fuzz smoke, build-breaking | **NOW** | — | 07 headline |
| 4 | Exemplars are tests (change #5) | `qa/` compile gate; fix or delete the 32 dead-API scripts; retire `vars_mixed.json` orphan | **NOW** | 3 | 11 F-grade, 07 |
| 5 | Plugin data contract, stage 1 (02) | Builder/extractor + key-constants headers for `mock_camera`, `blob_analysis` first (most-copied, worst hygiene), then the rest; fail-loud required inputs; schema-version stamp | **NOW** (additive headers) | — | 11 findings 2–3 |
| 6 | One template spine (change #5) | Collapse `easy`/`medium`/`expert` into one base-class skeleton with layered opt-ins | **CARVE** (SDK surface) | 5 (patterns settled) | 11 finding 3 |
| 7 | Contract-first schemas (change #1) | Spike the constrained JSON-Schema subset; describe the *current* wire as-is (non-breaking); generate fixtures from it | **CARVE** | schema-language decision | 06, 07, 10 |
| 8 | Shared client-core (change #1) | Generate/build `client-core-ts`; migrate extension → HMI → ui-components onto it; then `client-core-py` | **CARVE** | 7 | 10 headline |
| 9 | Extension consumes full event contract | Handle `run_result` / `state_dropped` / `compile_*` lifecycle in the extension | **CARVE** | 8 (cheapest after client-core, but doable before) | 10 finding 1 |
| 10 | Health/state contract (change #2, Bucket E) | Carve the state machine + component-health surface; new WS messages (additive); FE/HMI/extension consume it | **CARVE** | design pass | 01/02/03/05 4× ask |
| 11 | Blessed-concurrency-only (change #4) | JIT compiles scripts with raw-OpenMP rejected/flagged; docs route to `xi::parallel_for`/`xi::async`; breadcrumb slot recycling | **CARVE** | — | 08 findings |
| 12 | Project file version + full-document save (change #3) | Add `schema`/`version` field; rewrite `save_project` as read-modify-write preserving unknown keys | **CUTOVER** | — | 06 sharpest risk |
| 13 | Real protocol version (change #3) | Bump `abi` meaningfully; schema-diff gate enforces bumps; client-core enforces at `hello`; skew = explicit UX state | **CUTOVER** (enforcement) | 7, 8 | 06, 10 |
| 14 | Post-fault quarantine policy | Faulted instance marked degraded in health contract; explicit reuse policy | **CARVE** | 10 | 08 |
| 15 | Composed core (change #6) | Split `service_main.cpp` into subsystem owners | **DONE (v2, 2026-07-02)** — landed as the `refactor/split-service-main` + `refactor/split-plugin-manager` merges: `service_main.cpp` 5.2k→946 lines across `service_cmd_*` / `service_dispatch` / `service_inspect` / `service_toolchain` / `service_result` / `service_sinks` + `service_internal.hpp`; `xi_plugin_manager.hpp` → 57-line umbrella over `xi_pm_*` sub-headers. TU-level composition along the ideal's seams; the ownership-object model (RuntimeContext, Engine Stage 2) remains deferred per Bucket D | — | 02 I.10 triage |
| 16 | Plugin `describe()` schema publication (02 stage 2) | Carved interface + codegen for typed wrappers, config UI, Py typing | **GREENFIELD-ONLY for now** — revisit when stage-1 headers prove the pattern and a consumer (UI forms) exists | 5, 7 | 02 doc |

## Suggested sequence

Three waves, each independently shippable:

1. **Wave 1 — the NOW batch (items 1–5).** All non-breaking, all independent
   of each other and of any v3 decision. This wave alone retires the worst
   finding of reviews 07, 08, 09, and 11. It can start today and does not
   touch the app team.
2. **Wave 2 — the contract carve (items 6–11, after the schema-language
   spike).** The strangler move: `contract/` describes the current wire,
   client-core replaces the hand-rolled parsers one client at a time, the
   health contract gives every consumer one truth. No wire breaks yet — the
   schemas document what already flows.
3. **Wave 3 — the version-identity cutover (items 12–13).** Rides a
   coordinated cutover train with the app team — either joined to the
   already-staged breaking set or the next train after it. By then
   enforcement is cheap because client-core (wave 2) gives every client the
   check for free.

Item 16 is explicitly **not scheduled** — recorded so the question stays
closed until circumstances change (a real consumer for generated config UI).
Item 15, originally classed greenfield-only, was in fact adopted on v2 the
same day this map was written (see the table) — at the TU level, which is the
part that pays for itself; the ownership-object model stays deferred.

## What "done" looks like

Not "xInsp2 becomes xInsp3" — that was never the goal. Done is: **every
boundary in xInsp2 is either generated from one source or guarded by a gate**,
which is the actual content of the ideal. If a greenfield core is ever
undertaken afterwards, waves 1–3 mean the contract layer, the client fleet,
the gates, and the plugin data patterns all port unchanged — the greenfield
build would then be scoped to the core alone (item 15), at a fraction of
today's cost.

## Status — 2026-07-02, `polaris_master`

Executed as the Polaris line: 25 task branches merged into `polaris_master`
(never master), all verified green (backend ctest 59/59, plugins 3/3, pytest
42/42, extension typecheck/build + unit suites, `tools/gate.py` full pass:
docs / build / ctest / fixtures / qa / fuzz).

- **Done on polaris_master:** items 1–14 of the map (dispatch shell; pool/WS
  teardown safety; CI gate + qa quarantine registry; examples port + compile
  gate; plugin data contract stage 1; single template spine; contract/ schemas
  + validation gate + protocol baseline gate; client-core first steps (HMI on
  the shared shim, XEX1 goldens, py auth parity, extension events + skew UX);
  health contract + HMI/extension/FE/runner consumers; post-fault quarantine
  policy; project-file versioning + full-document save; blessed-concurrency
  guard; plus: doc-truth batch, doc-coverage extractor rebuild, hot-path bench,
  release compat manifest.
- **Item 12–13 note:** the full-document save + `xi.project/1` stamp and the
  protocol baseline gate are staged HERE (this branch is the staging line);
  the wire-visible halves (hello `abi` bump) still ride the app-team cutover.
- **Still open:** item 16 (schema-published `describe()` codegen, stage 2);
  the qa flaky/behavioral quarantine (4 entries in
  `qa/qa_known_failing.txt`) and the `ws_teardown_race` stress-test
  flake — the 07#8 deflake pass owns both.
