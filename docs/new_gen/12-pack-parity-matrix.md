# Pack Parity Matrix — the Gate P2 Measuring Stick

| Field | Value |
|---|---|
| **Date** | 2026-07-03 (re-measured after the three P2 write-half branches merged — see §The write half landed) |
| **Status** | LIVING INSTRUMENT — this document IS the Gate P2 gate (doc 10: "the pack path can express every pattern the guides teach, measured against the examples tree: every example expressible pack-only") |
| **Basis** | Everything marked GREEN is demonstrated on `polaris2_main` + this branch, in the RUNNING backend service (not host-mock), by a QA-gated example under `examples/qa_*` unless the evidence column says otherwise |
| **Method** | Every distinct pattern the examples tree (60 example dirs + 7 top-level scripts) and the plugin READMEs teach was enumerated (one row per pattern, examples mapped onto rows); each row was then tested against what the pack plane offers TODAY: `xi::Pack`/`PackBuilder` (xi_pack.hpp), the xi.pack@1 door (xi_pack_abi.hpp), source `emit_pack`, dual-carry dispatch, the script `t.pack()` read surface + `use().process(ScriptPack)` + `use(sink).push(ScriptPack)` (xi_use.hpp), and script-side building via `xi::ScriptPackBuilder` (xi_script_pack.hpp) |

## The write half landed (re-measure, 2026-07-03)

The three formerly IN-FLIGHT sibling branches are all merged on `polaris2_main`:

- **[USE-DOOR]** ✅ — `xi::use(name).process(ScriptPack)` (xi_use.hpp; service
  callback `use_pack_process_cb`, export `xi_script_set_use_pack_callback`).
- **[SCRIPT-BUILD]** ✅ — `xi::ScriptPackBuilder` → `seal()` → first-class
  `ScriptPack` (xi_script_pack.hpp), canonical-gated `add_mp` included (U4).
- **[EXPOSE-SCRIPT]** ✅ — `xi::use(sink).push(ScriptPack)` (xi_use.hpp; service
  callback `use_push_pack_cb`, export `xi_script_set_use_push_pack_callback`);
  declared sink targets are STAGED and flushed after the inspect in frame order.

The blocker tags below are retained in row history where useful but no longer
gate anything; rows were flipped ONLY where the evidence bar was met. A new
evidence class is used for rows whose two halves are proven separately:
**GREEN (composition)** = the script seam is live-QA-proven (qa_use_pack_door)
AND the target plugin's door is proven against the real built DLL, but no
single QA example composes them yet. Composition rows are named follow-ups,
not hidden debt.

## What "pack-only" means here (the honesty rules)

- The **per-frame data plane** — every payload that flows source → dispatch →
  script → operator → sink — is a sealed xi.pack@1 Pack. No `xi::Record`
  anywhere on the data path.
- The **control plane is out of scope**: exchange() JSON commands, params,
  project/instance config, run_result verdicts (`xi::ok/ng/result`), status
  breadcrumbs, dashboard/protocol/lifecycle machinery. These are not a data
  currency and survive Record deletion untouched. Rows that are pure control
  plane are N/A, listed at the bottom so the count is checkable.
- GREEN requires **evidence in the live service**, not aspiration: a QA-gated
  example (preferred) or, where explicitly noted, a shipped host-side test
  against the real DLL.
- The three sibling-branch blockers ([USE-DOOR] / [SCRIPT-BUILD] /
  [EXPOSE-SCRIPT]) have ALL LANDED (§The write half landed) — they appear
  below only as history inside flipped rows.
- A row blocked on work **nobody has scheduled** says so with a precise name
  (the §Unscheduled list — this is the wave-2 planning input).
- **[P3]** — Gate P3 persistence parity: ✅ ACHIEVED 2026-07-03 (doc 10);
  the tag survives only in E3's history.

## The reference pack-only examples (QA-gated, all green)

| Example | What it proves pack-only in the live service |
|---|---|
| `qa_pack_pilot` | mock_camera pack_mode → dual-carry dispatch → `t.pack()` reads seq + image dims → verdict. **Now pack-only end to end**: the expose leg (formerly the marked [EXPOSE-SCRIPT] Record gap) is a ScriptPackBuilder result pack pushed via `use("expose").push()` (ported 2026-07-03, still green). |
| `qa_pack_stereo` | synced_stereo pack_mode gathers left+right+seq into ONE sealed pack per trigger; script verifies both images' pixel-stamped seq equals the pack's `seq` entry (same-event correlation) — pack-only end to end, observed via the verdict plane alone (no Record anywhere). |
| `qa_pack_walk` | the script read surfaces beyond string gets: generic `for_each` walk with ABI tags, typed-schema (`ScriptTypedPack`) reads equal string-keyed reads, and a ScriptPack captured BY VALUE into a worker thread stays valid — pack-only, verdict-plane observed. |
| `qa_use_pack_door` **(new — the write-half flagship)** | the FULL P2 loop in one script: `xi::ScriptPackBuilder` assembles a white-square gray pack (image + scalars + a nested `xi::mp::Writer` entry round-tripped byte-identical — U4), chains it into blob_analysis's xi.pack@1 door via `xi::use("det").process(pack)` (blob_count/threshold_used/binary asserted off the returned ScriptPack), then builds a RESULT pack (derived binary image + nested mp + `$channel`/`$seq`) and `xi::use("expose").push()`es it, plus a second script-built pack on channel "qa2" (multi-channel). The driver decodes the XEX1-v3 wire and byte-checks the pushed pixels on both channels and the decoded nested entry; wire seq strictly increasing per channel (sink-staged flush). No `xi::Record` anywhere. |

## The matrix

Legend: **GREEN** = expressible pack-only today, live-service QA evidence.
**GREEN (composition)** = both halves proven separately (see §The write half
landed) — a named follow-up example is owed. Remaining blockers as defined
above.

### A. Source → dispatch → script (ingestion + reads)

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| A1 | Single-source per-frame **image** delivered to the script | burst_pipeline, image_sources, use_demo.cpp, qa_local_auto, qa_emit_frame_key | **GREEN** | `qa_pack_pilot` (dims through the door), `qa_pack_walk` (pixel reads). Source side: mock_camera `pack_mode` emits from its capture thread. |
| A2 | **Scalar/string metadata** payload delivered to the script | trigger_metadata (meta_source), json_source README | **GREEN** | `qa_pack_pilot`/`qa_pack_stereo`/`qa_pack_walk` (`seq` i64 entries). json_source's full JSON→pack mapping (scalars, bools, nested→mp, `$fault` on hostile input) is proven against the real DLL in `plugins/json_source/tests/test_json_source_pack.cpp`; the [USE-DOOR] that was blocking a script-side drive of its `process()` has landed (`use(name).process(ScriptPack)` live-proven in `qa_use_pack_door`), so the script-drive leg is now GREEN (composition). |
| A3 | **Gathered multi-image trigger** (stereo pair, one event) | stereo_sync (synced_cam), synced_stereo README | **GREEN** | `qa_pack_stereo` (new): one sealed pack carries `left`+`right`+`seq`; same-event correlation asserted in script hands. Note: gathering is the PLUGIN's job (one emitter, one pack) — the bus carries exactly one pack per event and has no bus-level multi-emitter pack merge; no example needs one (see §Explicit non-needs). |
| A4 | Trigger **identity / timing / ordered arrival** with data in flight | burst_pipeline (latency), qa_result_order (order) | **GREEN** | seq strict monotonicity through the ordered dispatch: `qa_pack_pilot` assert 2, `qa_pack_stereo` assert 3. `t.id()/timestamp_us()/dequeued_at_us()` are event fields, currency-independent. |
| A5 | **Generic producer-agnostic walk** (dump an unknown pack) | expose/record_save internals; the "generic sink" pattern of doc 02 r2 | **GREEN** | `qa_pack_walk` (new): script `for_each` + ABI tag checks — the same enumeration expose/record_save do host-side, now in script hands in-service. |
| A6 | **Typed-schema (declared keyset) reads** in the script | pack_pilot (manual), contract `_keys.h` pattern | **GREEN** | `qa_pack_walk` (new): `ScriptTypedPack` slot reads == string-keyed reads. Was previously taught only by the non-QA `examples/pack_pilot`. |
| A7 | **Cross-thread capture** of the payload (`xi::async` / `parallel_for` body) | parallel_inspect_demo, use_demo.cpp (snapshot discipline) | **GREEN** | `qa_pack_walk` (new): ScriptPack copied by value into a worker thread; worker/inspect checksums agree (the keepalive contract of xi_use.hpp, exercised in-service). |
| A8 | **Routing metadata** read off the trigger (`t.meta()`) | trigger_metadata | **GREEN** (pattern) | On the pack path metadata rides as pack ENTRIES (A2) — same information, one container. The `t.meta()` API itself returns a Record and retires at the cut (§Cut casualties). |
| A9 | **In-script pixel processing** (OpenCV etc.) over delivered frames | object_count_puzzle (input leg), qa_local_auto, defect_detection.cpp | **GREEN** (input leg) | `get_image()` yields a zero-copy `span` + dims — a `cv::Mat` wraps it directly; `qa_pack_walk` checksums prove the span read. The OUTPUT leg (pushing derived images) is D1. |

### B. Script → operator chaining

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| B1 | `use().process()` with **values**, read result fields | calc_pipeline, burst_dispatch, qa_func, qa_reentrancy | **GREEN** | `qa_use_pack_door` (new): a script-built pack through `xi::use("det").process(pack)` in the live service — blob_count/threshold_used read off the returned ScriptPack, `$fault` absent asserted. Full seam contract (result ownership, trigger chaining, $fault pack, fail-closed edges -1/-4/empty/older-host, refcount balance) in `plugins/use_pack_door_test.cpp` against the real DLLs. |
| B2 | `use().process()` with **images**, chain outputs zero-copy | circle_counting, circle_size_buckets, hue_tune, golden_defect, graph_demo, multi_source_surge | **GREEN** | `qa_use_pack_door`: image in (script-built gray), image back (the door's `binary`, byte-checked after a further push). Chaining a door RESULT onward is the same sealed handle passed as-is (`process` accepts any live ScriptPack — zero-copy across doors); note `ScriptPackBuilder::add_image` COPIES pixels by design (pool-identity across the script seam is deliberately not offered yet, xi_script_pack.hpp). |
| B3 | **Typed-IO contracts** (io.hpp build/extract) + **NA propagation** + **provenance** ($prov, out.src()) | fixturing_demo, io_stress, graph_demo (provenance) | **UNSCHEDULED U1** | The [USE-DOOR] half landed: the door is script-reachable and `$fault` packs are script-READABLE (`qa_use_pack_door` asserts fault=0 on the happy path; `use_pack_door_test` §4 reads a `$fault` pack). Still missing script-side: pack equivalents of `Record::na()/is_na()/na_reason()`, `$prov`/`prov_of()`, `set_src()/src()`, and `_io`-style typed pack build/extract helpers (doc 10 codegen gap #2 shipped `_io` only for the 3 keys-only plugins, Record-shaped). |
| B4 | **NA short-circuit** through a chain (poison input skips the plugin) | fixturing_demo, typed-io docs | **UNSCHEDULED U1** | The Record UseProxy short-circuits `is_na()` input before the door; the pack door has no NA notion to short-circuit on. Unchanged by the write-half landing — this is U1's propagation half. |

### C. Script → sink (observability + custom sinks)

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| C1 | Surface **values/images** on a channel (expose `$channel`) | virtually every example; qa_get_dashboard, qa_local_auto | **GREEN** | `qa_use_pack_door`: result pack pushed via `xi::use("expose").push()`, decoded off the XEX1-v3 wire (values + byte-checked image). `qa_pack_pilot`'s expose leg flipped to pack-only as promised (Record leg deleted 2026-07-03, still green). Host-side seam contract in `plugins/expose_script_push_test.cpp`. |
| C2 | **Multi-channel** preview (UI tabs) | preview_sink_demo | **GREEN** | `qa_use_pack_door`: a SECOND script-built pack (`$channel:"qa2"`, synthetic gray built with ScriptPackBuilder) pushed per frame; the driver subscribes both channels and byte-checks the qa2 image. Both landings arrived: `$channel` routing (door) + script-side building. |
| C3 | **Ordered sink** emission (`result_order`, wire `$seq`) | qa_result_order | **UNSCHEDULED U3** (narrowed) | RE-MEASURED: the ordered-STAGING analogue now EXISTS for `push()` — a declared sink target is staged and flushed after the inspect in frame order (`use_push_pack_cb`, service_sinks.cpp; `qa_use_pack_door` asserts wire seq strictly increasing per channel). What remains of U3: (a) the host does NOT stamp `$seq` into a sealed pack — ordering identity is script-carried by design (the pack's own `$seq` entry; expose defaults to 0 without it), so qa_result_order's host-stamped-arrival-id semantics need an owned decision (bless script-carried, or stamp at the door args/event); (b) `use().process()` on a sink target runs INLINE — the documented v0 gap (service_sinks.cpp) — so a pack-consuming ordered sink driven via process() would see completion order under parallel dispatch. Neither is exercised by any example today; both must be decided before qa_result_order's pattern ports. |
| C4 | **Custom sink instances** fed per-frame from the script | qa_sink_shared_doc (count_sink) | **GREEN** (pattern) | The feed leg landed: `qa_use_pack_door` feeds a sink instance (expose) per-frame via `push()` — the same xi.pack@1 door any custom sink implements (write one door, get the staged script feed for free). count_sink itself is Record-era and vehicle-ports at the cut; its `$seq` COW double-stamp regression RETIRES: sealed single-owner packs make the shared-doc double-stamp class unrepresentable (doc 10 §safety). |
| C5 | data_output config-surface sink | data_output README, image_sources | **N/A by design** | No process() override, no data plane (doc 10 footnote, verified 2026-07-03). |

### D. Script-BUILT payloads

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| D1 | Script constructs **derived images/values** and pushes them | object_count_puzzle (mask), defect_detection.cpp (6 previews), preview_sink_demo | **GREEN** | `qa_use_pack_door`: the blessed surface is `xi::ScriptPackBuilder` (xi_script_pack.hpp — host-side builder behind the xi.pack@1 door, canonical profile enforced, fail-closed on older hosts). Both derived-payload shapes live: a door-derived image re-packed + pushed (binary, byte-checked on the wire) and a fully synthetic script image (qa2). Backend seam unit: `test_script_pack`. |
| D2 | **Nested / grouped results** (per-ROI record arrays) | record_demo.cpp (`push("items", …)`), io_stress (nested typed) | **GREEN** (U4 SATISFIED) | `ScriptPackBuilder::add_mp(xi::mp::Writer)` IS the script-reachable canonical-mp writer U4 asked for — untrusted bytes go through `xi::mp::canonicalize` (reject-all ext policy), so a script cannot mint non-canonical pack bytes. Live evidence in `qa_use_pack_door`: a nested Writer map round-trips byte-identical through seal(), rides through the blob door input, and a copy on the RESULT pack decodes correctly off the XEX1-v3 wire ({origin, trigger_seq} checked by the driver). |

### E. Buffering / replay / persistence

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| E1 | **In-memory capture ring + hot-param replay** | buffer_replay_demo (cache), cache README, image_sources (cached_image_source) | **GREEN** | The owed composition landed as `examples/qa_pack_record_replay` (QA green 2026-07-03): both replay seams live-proven in one graph — script→door capture routing (`use(door).process(pack)` per trigger) and replayed-source-emit → `t.pack()` verification (the doc-10-named example E1/E2/E3 collapse into; scorecard note below). Cache's own ring specifics — capture door + ZERO-COPY replay re-emit (same handle, fresh trigger id) + eviction/teardown release — stay proven against the real DLL in `plugins/cache/tests/test_cache_pack.cpp` (re-run green 2026-07-03). |
| E2 | **Persist results to disk** | record_save README, data-out guides | **GREEN** | `examples/qa_pack_record_replay` phase 1 (QA green 2026-07-03): a live project routes camera packs into record_save's xi.pack@1 door from the script (`use("rec").process(cap)`, ack asserted) — one canonical XEX1-v3 file per trigger lands in the instance's captures folder, and the driver decodes the DISK files and proves disk == pushed wire, entry-for-entry and pixel-byte identical (memory≈wire≈disk, live). Byte-level oracle: `plugins/record_save_pack_test.cpp` (re-run green 2026-07-03). |
| E3 | **Replay FROM disk** (load a dump back into the pipeline) | record_save README (xex1_pack_load) | **GREEN** | `examples/qa_pack_record_replay` phase 2 (QA green 2026-07-03): a `record_replay` instance replays the phase-1 files into the SAME live graph (script pumps `use("replay").process(Record{})`, one file per synthetic tick); each file re-enters as a sealed-pack trigger (`t.primary_source()=="replay"`, the A1 path), the script verifies restored `$channel`/`$seq` + checksum + nested mp, and the driver proves replayed wire == disk == recorded wire with the cursor at position==total. Byte-level oracle: `plugins/record_replay_pack_test.cpp` (re-run green 2026-07-03). |

### F. Config-plane patterns over data-plane plugins

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| F1 | **Config swap** (prepare/commit) under live traffic | config_swap_probe README | **GREEN (composition)** | Record-vs-pack door parity of the probe proven in `plugins/config_swap_probe/tests/test_config_swap_probe_pack.cpp` (re-run green 2026-07-03); the script drive is the live-proven `use().process(pack)` seam. The observation surface (get_status) is control-plane JSON and unaffected. |
| F2 | **Retune-and-rerun** via exchange (hue_tune, golden_defect Param) | hue_tune, golden_defect, buffer_replay_demo | **N/A** (control) | exchange()/Params are control plane. The rerun's DATA leg is B2/E1 and inherits their status (B2 GREEN; E1 GREEN-composition). |

### G. Verdict / status / state

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| G1 | **Per-run verdicts** incl. ok/ng/na/crash classes | qa_run_result, defect_detection.cpp | **GREEN** (orthogonal) | The verdict plane is not a data currency; all four pack QA examples verdict while holding a pack. `qa_pack_stereo`/`qa_pack_walk` use it as the ONLY observability channel — proof it suffices pack-only. |
| G2 | **Sticky status** breadcrumbs (`xi::status`) | status_demo, crash_tests | **N/A** (control) | |
| G3 | **Cross-frame script state** (`xi::state()`) | blob_tracker, trend_monitor, hot_reload_run2 | **N/A today / UNSCHEDULED U2 at the cut** | Orthogonal to the payload currency (script-local), BUT `xi::state()` returns `xi::Record&` — it is a named casualty of Record deletion with no scheduled replacement. |
| G4 | **Params / recipes / instance defs** | qa_param_state_isolation, qa_recipe_script_instance, user_with_instance.cpp | **N/A** (control) | JSON config plane; explicitly untouched by the migration. |

### H. Pure control-plane examples (no per-frame data payload — N/A rows)

These teach lifecycle/dispatch/protocol/deployment patterns whose payloads are
empty or marker-only; the pack migration does not change what they express.
Where a Record appears it is a control vehicle, noted per family:

- **Dispatch/routing/limits**: qa_dispatch_groups, qa_group_parallelism,
  qa_group_stress, qa_two_group_paths, qa_min_interval, qa_cpu_affinity,
  parallel_inspect_demo, cross_proc_trigger, r6_p2_demo. (Trigger events carry
  packs identically — A4 covers the data-relevant part.)
- **Concurrency declaration**: qa_reentrancy (drives `use().process(Record)`
  as the vehicle; the declared-reentrancy semantics themselves are
  currency-independent; the vehicle re-tests under B1 when [USE-DOOR] lands).
- **Lifecycle/recovery**: qa_fault, qa_recover, qa_lifecycle_teardown,
  qa_watchdog, fe_supervisor, fe_supervisor_healthy, plugin_crash_forensics,
  crash_tests, qa_edge. (Note: the pack plane IMPROVES this family's story —
  drop-on-crash is destruction, doc 10 §6.)
- **Project/config persistence**: qa_corrupt_project_json, qa_working_copy,
  qa_instance_def_recompile, qa_recipe_script_instance, config_validation_demo.
- **Protocol/observability/deployment**: qa_get_dashboard, qa_runtime_settings,
  qa_export_bundle, qa_run_result (G1), status_demo, dll_version_clash,
  script_external_dll, multi_file_script, user_script_example.cpp,
  cancel_aware_script.cpp, hot_reload_run2 (state note → G3).
- **Record-era regression tests that RETIRE at the cut** (their failure class
  becomes unrepresentable): qa_sink_shared_doc (C4), qa_emit_frame_key (the
  event image-map "frame" key semantics — pack images ride IN the pack, the
  event map is Record machinery).
- **Vehicle-ported at the cut**: qa_func, qa_local_auto, qa_emit_frame_key,
  qa_result_order and every H-family script that touches `expose` re-writes its
  Record leg to the pack equivalents (C1/B1) on the cut train — doc 10 step 4
  "port the examples tree".

## Scorecard (re-measured 2026-07-03, post-landing)

Counting the 29 pattern rows (A1–A9, B1–B4, C1–C5, D1–D2, E1–E3, F1–F2, G1–G4;
§H is the N/A block — the pre-landing scorecard said "30", a miscount):

| Status | Rows | Count |
|---|---|---|
| **GREEN (pack-only, live-service QA evidence)** | A1–A9, B1, B2, C1, C2, C4, D1, D2, E1, E2, E3, G1 | **20** |
| **GREEN (composition — both halves proven, one example owed)** | F1 | **1** |
| **Open on unscheduled semantics** | B3, B4 (U1), C3 (U3) | **3** |
| **N/A (control plane / by design)** | C5, F2, G2, G4 + the §H block | **4 rows + H** |
| **N/A today / U2 at the cut** | G3 | **1** |

Bottom line for Gate P2: **both halves of script parity are now landed and
QA-gated** — read (t.pack(), walk, typed, cross-thread) AND write
(ScriptPackBuilder incl. canonical nested-mp, use()→door chaining,
expose-from-script with staged sink ordering), the full loop proven in one
live script (`qa_use_pack_door`). U4 is SATISFIED. What remains is exactly
two named semantic residuals — **U1** (pack-plane NA/provenance/typed-IO:
gates the error paths of fixturing_demo / io_stress / graph_demo, rows B3/B4)
and **U3** (ordered-sink semantics: script-carried `$seq` needs blessing, and
process()-on-sink runs inline — gates qa_result_order's pattern, row C3) —
plus one composition row still owing its example (F1: a pack-mode config-swap
drive). The graph-level record→replay example doc 10 named — E1/E2/E3's shared
debt — LANDED 2026-07-03 as `examples/qa_pack_record_replay` (QA green:
record → save(.xex1) → replay → verify as one live graph, disk == recorded
wire == replayed wire pixel-byte identical), flipping those three rows GREEN.
**The Gate P2 verdict rendered on this measurement is in
doc 10: ACHIEVED WITH NAMED RESIDUALS (U1, U3).** U2 (`xi::state()`'s Record
shape) gates the CUT, not P2.

## Unscheduled blockers (the wave-2 planning input; re-measured 2026-07-03)

- **U1 — Pack-plane NA / provenance / typed-IO script semantics. STILL OPEN.**
  No pack equivalent of `Record::na()/is_na()/na_reason()` (NA propagation
  through chains), `$prov`/`prov_of()`, `set_src()/src()` (provenance), and no
  `_io`-style typed build/extract helpers for packs beyond doc 10's codegen
  gap #2 (keys-only plugins, Record-shaped). What the landing DID give:
  plugin-side `$fault` packs are now script-READABLE (`use_pack_door_test` §4)
  — the fail-loud seed exists; the propagate/short-circuit contract is
  unowned. Affects: fixturing_demo, io_stress, graph_demo, and the error path
  of every chained example. **This is Gate P2 residual #1 (doc 10).**
- **U2 — `xi::state()` post-Record shape. STILL OPEN (verified 2026-07-03:
  xi_state.hpp still returns `xi::Record&`).** DocRegistry/COW die at the cut.
  Keep-as-JSON vs move-to-pack is nobody's decision yet. Affects:
  blob_tracker, trend_monitor, hot_reload_run2 (+ `t.meta()`'s Record return,
  same family). Gates the CUT, not P2.
- **U3 — Ordered-sink semantics on the pack plane. NARROWED, still open.**
  The staging analogue landed for `push()` (sink targets staged + flushed in
  frame order — live-evidenced by strictly-increasing wire seq in
  `qa_use_pack_door`). Remaining: (a) hosts do not stamp `$seq` into sealed
  packs — ordering identity is script-carried by design; bless that (and
  give qa_result_order's host-stamped semantics an explicit successor) or
  specify a door-args/event carry; (b) `use().process()` on a sink target
  runs INLINE (documented v0 gap, service_sinks.cpp) — completion-order side
  effects under parallel dispatch. Affects: qa_result_order's pattern.
  **This is Gate P2 residual #2 (doc 10).**
- **U4 — Script-reachable canonical-mp writer for nested entries.
  ✅ SATISFIED 2026-07-03** by `ScriptPackBuilder::add_mp(xi::mp::Writer)`
  (xi_script_pack.hpp): nested bytes pass the `xi::mp::canonicalize`
  reject-all-ext gate, canonical input rides byte-identical. Live evidence:
  `qa_use_pack_door` (round-trip in script + decode off the v3 wire); unit:
  `test_script_pack`. [SCRIPT-BUILD] scoped it in, exactly as flagged.

## Explicit non-needs (checked, deliberately NOT scheduled)

- **Bus-level multi-emitter pack merge**: the bus carries exactly one pack per
  event (`TriggerEvent::pack`); gathering is the emitting plugin's job
  (synced_stereo). No example teaches bus-policy multi-source pack
  correlation. If a future example needs it, it is new design work — not a
  forgotten port.
- **Scriptless source→sink pack routing** (dispatch calling instance pack
  doors directly): the service delivers packs only to the script; every sink
  in the examples tree is script-fed. [USE-DOOR] covers the tree. Noted so its
  absence is a decision, not an oversight.

## Operational note (hit during this work)

`plugins/build` is a SEPARATE cmake tree from `backend/build`; `tools/gate.py
build` builds both, but a manual backend-only rebuild leaves stale plugin DLLs
in `plugins/<name>/` — and because `set_def` ignores unknown keys, a stale
pre-bilingual DLL silently downgrades `pack_mode:true` to Record emission
(observed live: `get_def` missing the `pack_mode` key is the tell; the
pack-only QA examples then fail loudly with "no verdict", which is the
designed fail-loud, not a silent pass).

## Row-flip protocol

When a sibling branch lands: re-run `python tools/run_qa.py pack`, add/extend
QA examples for the newly-expressible rows (B/C/D rows each name their
example-to-be), change the row's status to GREEN with the example as evidence,
and update the scorecard. A row may NOT be flipped on the strength of a design
doc or a host-mock test alone — live-service QA evidence only, same bar the
GREEN rows above met.
