# Frame Plane — Migration Scope Decision (Wave-2 Exit)

| Field | Value |
|---|---|
| **Date** | 2026-07-03 |
| **Status** | DRAFT for maintainer decision — the wave-2 exit-gate deliverable (docs/new_gen/08) |
| **Basis** | Everything below is merged and gate-green on `polaris2_main` |

## What the pilot proved (evidence, not claims)

1. **Performance**: TypedFrame 522 ns/op vs Record 924 on the identical
   metadata workload (43%); dispatch p99 at parallel=8 tightens ~33µs → ~22µs
   (bench_frame, dev-box medians; perf-runner baseline still to capture).
2. **Identity**: the in-memory small plane IS the wire IS the disk —
   `xex1_v2_identity_test` asserts the three byte-equalities.
3. **Genericity** (02's r2 constraint): `expose` walks any producer's Frame
   with zero producer knowledge; the contour rides as nested canonical mp.
4. **End-to-end**: mock_camera(frame_mode) → dual-carry dispatch →
   `t.frame()` script → verdict, as a QA-gated example (`qa_frame_pilot`).
5. **Contract discipline held under fire**: the codegen drift gate caught a
   real cross-branch drift (frame_mode/seq) the day it landed; generated
   headers replaced hand-written ones with zero call-site edits.
6. **Safety story simplified**: sealed frames = drop-on-crash; the
   leak-per-crash class is unrepresentable on the frame path (the Record
   path's leak is now counted: `crash_leaked_docs_lifetime`).

## Proposed migration scope

### Lands on `polaris2_main` (no wire break, no app-team dependency)

| What | When | Cost basis |
|---|---|---|
| Remaining plugins go BILINGUAL (json_source, synced_stereo, config_swap_probe, cache, record_save, data_output — sinks get the generic walk, sources the emit mode) | next batch; ~½–1 day each (measured: blob +77 lines, mock +29) | pilots' diffs |
| `xi::use()` → frame-door plumbing for scripts (w2c's honest v0 gap: scripts can read t.frame() but not yet drive an operator's door) | one focused task; unblocks script-side chaining | frame_pilot_test shows the host-side call pattern |
| Codegen gap #2 (reply-extractor family, param defaults, has-accessors) → the 3 keys-only plugins get generated `_io` too | one generator task | w2d's gap report |
| Record→Frame conversion shims (explicit, opt-in helpers — never silent) | decide AFTER the bilingual batch; may prove unnecessary | — |
| Perf-runner baseline capture for perf_frame + perf_hotpath | before any master-merge conversation | benches ship SKIP-until-fingerprinted |

### Rides the app-team cutover train (wire-visible; bundle with the `abi` bump)

- **XEX1-v2 becomes the default** preview encoding (v1 retained one release
  behind a flag, then removed). Clients: expose webUI + xex1.py already
  bilingual; third-party decoders get the golden fixtures.
- `hello.abi` 1 → 2 (the long-planned stamp bump; extension already enforces).
- Anything the bilingual batch surfaces as genuinely shape-breaking (none
  known today).

### Record deprecation horizon (proposal)

- **Now → bilingual-complete**: Record is the default currency; Frame is the
  pilot surface. No deprecation language anywhere.
- **Bilingual-complete + script chaining shipped**: Record path enters
  **maintenance** (bugfix-only; new capabilities land frame-first). The
  write-a-script/plugin guides teach Frame as primary.
- **Hard removal**: NOT on xInsp2. Record deletion is a v3/greenfield event
  (or a deliberate 1.0 break with the app team) — the dual-carry and
  bilingual pattern are cheap to keep until then, and honesty beats a forced
  march.

## Open decisions for the maintainer

- [ ] Approve the bilingual batch (6 plugins) as the next polaris2_main wave.
- [ ] Approve "maintenance mode" as Record's end state on v2 (vs. scheduling
      hard removal on a 1.0 cutover).
- [ ] XEX1-v2 default flip: joined to the abi-bump train, or its own later
      train? (Recommend: same train — one client-visible moment, not two.)
- [ ] When to capture perf baselines (which machine is the blessed runner).
- [ ] Whether `polaris2_main` folds back into `polaris_master` (recommended:
      yes, after the bilingual batch — one integration line, fewer moving
      parts) and when THAT line goes to master with the app team.
