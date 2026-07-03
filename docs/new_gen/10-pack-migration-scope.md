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
| ~~`xi::use()` → pack-door plumbing for scripts~~ **DONE** (polaris2/p2-use-door): `xi::use(name).process(ScriptPack)` chains a pack (t.pack() or a built one) into a plugin's xi.pack@1 door via the new optional `xi_script_set_use_pack_callback` export + host `use_pack_process_cb` (service + runner); Record path byte-untouched; pinned by `use_pack_door_test`. v0 gap: pack calls on ordered-SINK targets run inline (no frame-ordered staging) — revisit when a pack-consuming sink exists | one focused task; unblocks script-side chaining | frame_pilot_test shows the host-side call pattern |
| **DONE (2026-07-03)** Codegen gap #2 (reply-extractor family, param defaults, has-accessors + raw-JSON/patch_builder shapes) → the 3 keys-only plugins are full swaps (generated `_io`, hand-written `_io.h` deleted; see `contract/codegen/README.md` "Coverage") | one generator task | w2d's gap report |
| Record→Pack conversion shims (explicit, opt-in helpers — never silent) | decide AFTER the bilingual batch; may prove unnecessary | — |
| Perf-runner baseline capture for perf_frame + perf_hotpath | before any master-merge conversation | benches ship SKIP-until-fingerprinted |

### Rides the app-team cutover train (wire-visible; bundle with the `abi` bump)

- **XEX1-v3 becomes the default** preview encoding (v1 retained one release
  behind a flag, then removed). v3 supersedes the tagless v2 draft — finalized
  with per-entry tags at gate P3, BEFORE any v2 file shipped, so the flip is
  v1→v3 with no intermediate. Clients: expose webUI + xex1.py already
  bilingual (v1+v3); third-party decoders get the golden fixtures.
- `hello.abi` 1 → 2 (the long-planned stamp bump; extension already enforces).
- Anything the bilingual batch surfaces as genuinely shape-breaking (none
  known today).

### Record removal schedule (maintainer decision 2026-07-03: DELETE, scheduled)

Keeping two currencies indefinitely is the hand-synced-two-representations
disease at the API layer, and Record's survival keeps DocRegistry, COW,
share_out/adopt, and the counted crash-leak machinery alive forever. Record
is DELETED from xInsp2, gated on parity milestones — a deliberate pre-1.0
break in the VAR-hard-delete tradition, not a drift into permanence:

1. **Gate P1 — plugin parity: ✅ ACHIEVED 2026-07-03.** All shipped plugins
   with a data plane are bilingual[^data_output]: expose/blob_analysis/
   mock_camera (pilot) + json_source, synced_stereo, config_swap_probe, cache,
   record_save (bilingual batch) — each landed with parity / gathering /
   zero-copy-replay / persistence-identity proofs. Bonus findings on the
   pack-plane hardening list: PackRegistry owner-sweep asymmetry (cache) —
   **FIXED** (owner-tagged ref ledger + `release_all_for` + "swept N leaked
   pack ref(s)" diagnostic at adapter dtor / script unload; regression test in
   `test_pack_door`); PackBuilder bool-entry gap (json_source) — **FIXED**
   (PackTag::Bool end-to-end: builder → canonical 0xc2/0xc3 → walk/dump/load →
   typed accessors → additive `xi_pack_v1` tail; json_source/record_save now
   emit real bools; `v3_bool` golden fixture cross-checked by the JS + Python
   decoders); v2-dump image-descriptor shape ambiguity (record_save) —
   **CLOSED** by the v3 per-entry-tag format at gate P3.

[^data_output]: `data_output` is a config-surface teaching example — it overrides
    no `process()` path, so it has no data plane and the Pack door is N/A by
    design (verified 2026-07-03). It carries no parity obligation; see its README
    and `expose` for the generic-sink reference.
2. **Gate P2 — script parity: ✅ ACHIEVED 2026-07-03, WITH TWO NAMED
   RESIDUALS (U1, U3).** All three surfaces landed and are QA-gated in the
   live service: `use()`→door chaining (`xi::use(name).process(ScriptPack)`),
   script-side Pack building (`xi::ScriptPackBuilder`, whose canonical-gated
   `add_mp(xi::mp::Writer)` closes the U4 nested-results gap), and
   expose-from-script (`xi::use(sink).push(ScriptPack)`, staged + flushed in
   frame order for sink targets). Measured against the examples tree via the
   re-measured parity matrix (doc 12, 2026-07-03): 17 of 29 pattern rows
   GREEN on live QA evidence, 4 GREEN-composition (both halves proven, one
   composing example owed: E1/E2/E3 collapse into the graph-level
   record→replay example below, F1 into a pack-mode config-swap drive —
   F1's is DELIVERED 2026-07-03: **`examples/qa_pack_config_swap`** drives
   the probe's pack door under live mock_camera traffic while the driver
   runs `prepare_instance` → `commit_group` mid-run, verdicts on the
   run_result plane, zero `xi::Record`; row F1 flipped GREEN in doc 12), 4+H
   N/A control-plane. Flagship evidence: **`examples/qa_use_pack_door`** —
   build → door → push in ONE script, no `xi::Record` anywhere, pixels and
   nested entries byte-checked off the XEX1-v3 wire; `qa_pack_pilot`'s last
   Record leg deleted the same day; `python tools/run_qa.py pack` 4/4.
   The residuals, named precisely (doc 12 §Unscheduled has the detail):
   - **U1 — pack-plane NA/provenance/typed-IO script semantics** (no
     `na()/is_na()/na_reason()`, `$prov`, `src()`, or `_io`-style pack
     helpers; `$fault` packs are script-readable, the propagate contract is
     unowned). Gates the ERROR-PATH patterns of fixturing_demo, io_stress,
     graph_demo — matrix rows B3/B4.
   - **U3 — ordered-sink semantics** (hosts stamp no `$seq` into sealed
     packs — script-carried ordering needs blessing or a door-args carry;
     and `use().process()` on a sink target runs inline, the documented v0
     gap). Gates qa_result_order's host-stamped pattern — matrix row C3.
   Neither residual blocks any shipping example pattern beyond those named;
   both are wave-2 planning inputs. **The CUT (step 4) may not ride until
   U1/U3 have owners or explicit won't-need decisions** — and U2 below is a
   separate cut-gate. (U2 — `xi::state()` still returns `xi::Record&` — is
   NOT a P2 residual: cross-frame state is script-local and orthogonal to
   the payload currency, but it must be resolved before Record deletion.)
3. **Gate P3 — persistence parity: ✅ ACHIEVED 2026-07-03.** The canonical
   dump is FINALIZED as **XEX1-v3** (= the v2 draft + per-entry
   `XI_PACK_TAG_*` as `[tag, value]`, closing P1's image-descriptor
   shape-ambiguity bonus finding before any v2 file shipped; loaders now
   recover entry types exactly, never by shape, and refuse the tagless
   draft fail-closed). `record_save` persists it; the new **`record_replay`
   source** re-emits it through the pack door — record → save → replay is a
   closed, byte-lossless loop (`record_replay_pack_test`: replayed pack
   entry-for-entry identical incl. restored `$channel`/`$seq`, re-dump ==
   disk == original's dump; `xex1_v2_identity_test` still pins
   memory≈wire≈disk — historical name, v3 bytes). The migration note for
   pre-v3 artifacts is `13-replay-file-migration.md` (headline: nothing
   pre-v3 ever had a replay path, so v3 breaks none; the one open item —
   an optional .json/.bmp→.xex1 converter — is parked with the app team on
   the cutover train). Remaining P3-adjacent work, tracked outside the
   gate: flipping expose's default wire to v3 rides the cutover train
   (below), and the graph-level "route a recorded run through a full
   project" example is examples-tree work under gate P2's umbrella.
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
