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
