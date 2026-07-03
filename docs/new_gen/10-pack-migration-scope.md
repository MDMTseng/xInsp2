# Pack Plane — Migration Scope Decision (Wave-2 Exit)

> **Naming note:** the container was renamed **Frame → Pack** after this
> document was first drafted (zero image connotation; see doc 07). Container
> tokens below are updated; quoted test/bench names from the pilot era keep
> their historical spellings.

| Field | Value |
|---|---|
| **Date** | 2026-07-03 |
| **Status** | DRAFT for maintainer decision — the wave-2 exit-gate deliverable (docs/new_gen/08) |
| **Basis** | Everything below is merged and gate-green on `polaris2_main` |

## What the pilot proved (evidence, not claims)

1. **Performance**: TypedPack 522 ns/op vs Record 924 on the identical
   metadata workload (43%); dispatch p99 at parallel=8 tightens ~33µs → ~22µs
   (bench_frame, dev-box medians; perf-runner baseline still to capture).
2. **Identity**: the in-memory small plane IS the wire IS the disk —
   `xex1_v2_identity_test` asserts the three byte-equalities.
3. **Genericity** (02's r2 constraint): `expose` walks any producer's Pack
   with zero producer knowledge; the contour rides as nested canonical mp.
4. **End-to-end**: mock_camera(pack_mode) → dual-carry dispatch →
   `t.pack()` script → verdict, as a QA-gated example (`qa_pack_pilot`).
5. **Contract discipline held under fire**: the codegen drift gate caught a
   real cross-branch drift (frame_mode/seq) the day it landed; generated
   headers replaced hand-written ones with zero call-site edits.
6. **Safety story simplified**: sealed packs = drop-on-crash; the
   leak-per-crash class is unrepresentable on the pack path (the Record
   path's leak is now counted: `crash_leaked_docs_lifetime`).

## Proposed migration scope

### Lands on `polaris2_main` (no wire break, no app-team dependency)

| What | When | Cost basis |
|---|---|---|
| Remaining plugins go BILINGUAL (json_source, synced_stereo, config_swap_probe, cache, record_save, data_output — sinks get the generic walk, sources the emit mode) | next batch; ~½–1 day each (measured: blob +77 lines, mock +29) | pilots' diffs |
| `xi::use()` → pack-door plumbing for scripts (w2c's honest v0 gap: scripts can read t.frame() but not yet drive an operator's door) | one focused task; unblocks script-side chaining | frame_pilot_test shows the host-side call pattern |
| Codegen gap #2 (reply-extractor family, param defaults, has-accessors) → the 3 keys-only plugins get generated `_io` too | one generator task | w2d's gap report |
| Record→Pack conversion shims (explicit, opt-in helpers — never silent) | decide AFTER the bilingual batch; may prove unnecessary | — |
| Perf-runner baseline capture for perf_frame + perf_hotpath | before any master-merge conversation | benches ship SKIP-until-fingerprinted |

### Rides the app-team cutover train (wire-visible; bundle with the `abi` bump)

- **XEX1-v2 becomes the default** preview encoding (v1 retained one release
  behind a flag, then removed). Clients: expose webUI + xex1.py already
  bilingual; third-party decoders get the golden fixtures.
- `hello.abi` 1 → 2 (the long-planned stamp bump; extension already enforces).
- Anything the bilingual batch surfaces as genuinely shape-breaking (none
  known today).

### Record removal schedule (maintainer decision 2026-07-03: DELETE, scheduled)

Keeping two currencies indefinitely is the hand-synced-two-representations
disease at the API layer, and Record's survival keeps DocRegistry, COW,
share_out/adopt, and the counted crash-leak machinery alive forever. Record
is DELETED from xInsp2, gated on parity milestones — a deliberate pre-1.0
break in the VAR-hard-delete tradition, not a drift into permanence:

1. **Gate P1 — plugin parity**: all 9 shipped plugins bilingual (the batch
   above).
2. **Gate P2 — script parity**: `use()`→door chaining + script-side Pack
   building + expose-from-script land; the pack path can express every
   pattern the guides teach (measured against the examples tree: every
   example expressible pack-only).
3. **Gate P3 — persistence parity**: record_save/replay v2 = canonical pack
   dump (memory≈wire≈disk already proves the format); a migration note for
   existing replay files.
4. **THE CUT (one event, with the app team)**: ABI v12 — recommended as the
   synthesis §3 pure-door ABI (delete the monolith struct in the same
   authorized break), delete `xi_plugin_process(Record)`, delete Record +
   DocRegistry + COW + share/adopt (+ its crash-leak counter, made obsolete),
   port the examples tree, app-team scripts port on the same train as the
   XEX1-v2 default + `abi` bump. One coordinated moment, not three.
- Until P1–P3 are green, NO deprecation language anywhere (a half-deprecated
  API teaches worse than either state).

## Open decisions for the maintainer

- [ ] Approve the bilingual batch (6 plugins) as the next polaris2_main wave.
- [x] Record end state: DELETE, scheduled behind parity gates P1-P3 with the
      cut riding the 1.0/abi cutover train (decided 2026-07-03; supersedes
      the earlier maintenance-mode draft).
- [ ] XEX1-v2 default flip: joined to the abi-bump train, or its own later
      train? (Recommend: same train — one client-visible moment, not two.)
- [ ] When to capture perf baselines (which machine is the blessed runner).
- [ ] Whether `polaris2_main` folds back into `polaris_master` (recommended:
      yes, after the bilingual batch — one integration line, fewer moving
      parts) and when THAT line goes to master with the app team.
