# new_gen — xInsp3 planning

Planning documents for "xInsp3" — the **ideal form** of the system under the
same philosophy as xInsp2 (speed-first, minimal core,
functionality-as-plugins), incorporating the structural lessons from the
external reviews in [`../ext_review/`](../ext_review/).

The ideal is a north star, not a rewrite commitment: `01`/`02` describe what
the architecture *should* look like free of patch-by-patch thinking; `03`
maps which parts get scheduled onto the real xInsp2, in what order, and which
parts are explicitly greenfield-only.

## Documents

| Order | Document | Content |
|---:|---|---|
| 1 | [`01-xinsp3-architecture.md`](./01-xinsp3-architecture.md) | The main architecture proposal: unchanged spine, the six structural changes (contract-first codegen, core-owned health contract, version identity on every boundary, structural failure boundaries, gates/exemplars as day-1 infrastructure, composed core), system picture, repo layout, what stays out, build order, decision checklist |
| 2 | [`02-plugin-data-contract.md`](./02-plugin-data-contract.md) | Design decision: plugin data stays schemaless `Record` across the ABI; typed structs-across-ABI rejected. Adopted stage 1: hand-written input-builder / output-extractor headers over shared key constants, fail-loud required inputs, schema-version stamp; stage 2 generates the same headers (plus config UI, Python typing, docs) from a published schema |
| 3 | [`03-adoption-map.md`](./03-adoption-map.md) | Maps the ideal onto xInsp2 as schedulable increments: NOW / CARVE / CUTOVER / GREENFIELD-ONLY classification per element, three shippable waves, and what is explicitly not scheduled |
| 4 | [`04-health-contract.md`](./04-health-contract.md) | The core-owned health/state contract as designed and implemented (schema `xi.health/1`): state machine, derived component model, wire surface, quarantine policy, non-goals |
| 5 | [`05-schema-language-spike.md`](./05-schema-language-spike.md) | Schema-language spike findings and recommendation: constrained JSON-Schema subset adopted for the text wire; where JSON Schema fought back (unions, binary framing, int64) and the resolutions |
| 6 | [`06-app-team-migration-plan.md`](./06-app-team-migration-plan.md) | Migration plan for the app team: impact classification (4 action items, behavior fixes, additive surfaces), phases 0–3 (inventory → merge → adopt → `abi` bump cutover), known issues, rollback |
| 7 | [`polaris2/`](./polaris2/00-synthesis.md) | Polaris2 check: four independent architects (no access to new_gen/ext_review) each verified polaris_master and designed their ideal next generation; `00-synthesis.md` compares them against the north star — unanimous validation of contract-first, plus two amendments (immutable tagged arena for per-frame metadata; pure `get_interface`, no monolith struct) and a converged check-findings triage |
| 8 | [`07-uniform-keyed-buffer-plane.md`](./07-uniform-keyed-buffer-plane.md) | Adopted v3 data-plane direction: one frame container of keyed `(type tag, binary buffer)` entries — no image/meta split; everything structured (incl. image/tensor descriptors) encodes as msgpack under a canonical max-width profile — fixed-width numbers give O(1) offset reads and one decoder path (memory ≈ wire ≈ disk); pixels stay raw in aligned pool buffers behind a handle ext type |
| 9 | [`08-polaris2-main-plan.md`](./08-polaris2-main-plan.md) | Implementation plan for `polaris2_main`: strangler round two — wave 0 (deferred polaris2 findings incl. the live-wire conformance test), wave 1 (canonical codec, Frame container, ingress canonicalizer, the GO/NO-GO benchmark), wave 2 (`xi.frame@1` carved pilot on mock_camera→blob_analysis, memory≈wire≈disk proof), wave 3 (contract codegen replacing hand-written accessors) |
| 10 | [`10-frame-migration-scope.md`](./10-frame-migration-scope.md) | Wave-2 exit deliverable (DRAFT for decision): what the frame pilot proved, the proposed migration scope (bilingual batch on polaris2_main; XEX1-v2 + abi bump on the cutover train), Record's maintenance-mode horizon, open maintainer decisions |
