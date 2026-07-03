# Pack Parity Matrix — the Gate P2 Measuring Stick

| Field | Value |
|---|---|
| **Date** | 2026-07-03 |
| **Status** | LIVING INSTRUMENT — this document IS the Gate P2 gate (doc 10: "the pack path can express every pattern the guides teach, measured against the examples tree: every example expressible pack-only") |
| **Basis** | Everything marked GREEN is demonstrated on `polaris2_main` + this branch, in the RUNNING backend service (not host-mock), by a QA-gated example under `examples/qa_*` unless the evidence column says otherwise |
| **Method** | Every distinct pattern the examples tree (60 example dirs + 7 top-level scripts) and the plugin READMEs teach was enumerated (one row per pattern, examples mapped onto rows); each row was then tested against what the pack plane offers TODAY: `xi::Pack`/`PackBuilder` (xi_pack.hpp), the xi.pack@1 door (xi_pack_abi.hpp), source `emit_pack`, dual-carry dispatch, and the script `t.pack()` read surface (xi_use.hpp) |

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
- A row blocked on one of the three IN-FLIGHT sibling branches names it, so
  the matrix mechanically flips as each lands:
  - **[USE-DOOR]** — `xi::use()` → pack-door chaining for scripts
  - **[SCRIPT-BUILD]** — script-side Pack building
  - **[EXPOSE-SCRIPT]** — expose-from-script on the pack plane
- A row blocked on work **nobody has scheduled** says so with a precise name
  (the §Unscheduled list — this is the wave-2 planning input).
- **[P3]** — blocked on Gate P3 persistence parity (scheduled in doc 10,
  not started).

## The reference pack-only examples (QA-gated, all green)

| Example | What it proves pack-only in the live service |
|---|---|
| `qa_pack_pilot` | mock_camera pack_mode → dual-carry dispatch → `t.pack()` reads seq + image dims → verdict. (Its observability leg still re-surfaces values through a Record on expose — the [EXPOSE-SCRIPT] gap, marked below.) |
| `qa_pack_stereo` **(new)** | synced_stereo pack_mode gathers left+right+seq into ONE sealed pack per trigger; script verifies both images' pixel-stamped seq equals the pack's `seq` entry (same-event correlation) — pack-only end to end, observed via the verdict plane alone (no Record anywhere). |
| `qa_pack_walk` **(new)** | the script read surfaces beyond string gets: generic `for_each` walk with ABI tags, typed-schema (`ScriptTypedPack`) reads equal string-keyed reads, and a ScriptPack captured BY VALUE into a worker thread stays valid — pack-only, verdict-plane observed. |

## The matrix

Legend: **GREEN** = expressible pack-only today. Blockers as defined above.

### A. Source → dispatch → script (ingestion + reads)

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| A1 | Single-source per-frame **image** delivered to the script | burst_pipeline, image_sources, use_demo.cpp, qa_local_auto, qa_emit_frame_key | **GREEN** | `qa_pack_pilot` (dims through the door), `qa_pack_walk` (pixel reads). Source side: mock_camera `pack_mode` emits from its capture thread. |
| A2 | **Scalar/string metadata** payload delivered to the script | trigger_metadata (meta_source), json_source README | **GREEN** | `qa_pack_pilot`/`qa_pack_stereo`/`qa_pack_walk` (`seq` i64 entries). json_source's full JSON→pack mapping (scalars, bools, nested→mp, `$fault` on hostile input) is proven against the real DLL in `plugins/json_source/tests/test_json_source_pack.cpp`; its in-service emit fires inside `process()`, so driving it from a script is Record-carried until [USE-DOOR] lands (flips fully then). |
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
| B1 | `use().process()` with **values**, read result fields | calc_pipeline, burst_dispatch, qa_func, qa_reentrancy | **[USE-DOOR]** | Operator door is ready: blob_analysis `process(PackIn&,PackOut&)` + the chained flow (source pack → door) proven host-side in `plugins/pack_pilot_test.cpp`. Scripts cannot reach any instance door — `UseProxy::process` is Record-only. |
| B2 | `use().process()` with **images**, chain outputs zero-copy | circle_counting, circle_size_buckets, hue_tune, golden_defect, graph_demo, multi_source_surge | **[USE-DOOR]** | Same as B1; `adopt_image` in the builder ABI gives the zero-copy leg. |
| B3 | **Typed-IO contracts** (io.hpp build/extract) + **NA propagation** + **provenance** ($prov, out.src()) | fixturing_demo, io_stress, graph_demo (provenance) | **[USE-DOOR]** + **UNSCHEDULED U1** | Plugin-side fail-loud exists ($fault packs: blob_analysis, json_source). Script-side there is NO pack equivalent of `Record::na()/is_na()/na_reason()`, `$prov`/`prov_of()`, `set_src()/src()`, and no `_io`-style typed pack build/extract helpers (doc 10 codegen gap #2 schedules `_io` only for the 3 keys-only plugins). |
| B4 | **NA short-circuit** through a chain (poison input skips the plugin) | fixturing_demo, typed-io docs | **[USE-DOOR]** + **UNSCHEDULED U1** | The Record UseProxy short-circuits `is_na()` input before the door; the pack door has no NA notion to short-circuit on. |

### C. Script → sink (observability + custom sinks)

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| C1 | Surface **values/images** on a channel (expose `$channel`) | virtually every example; qa_get_dashboard, qa_local_auto | **[EXPOSE-SCRIPT]** | expose's pack door + generic walk + XEX1-v1/v2 encode are ready (`plugins/expose_pack_test.cpp`); what's missing is the script→expose pack leg. `qa_pack_pilot`'s expose leg flips to pack-only when this lands. |
| C2 | **Multi-channel** preview (UI tabs) | preview_sink_demo | **[EXPOSE-SCRIPT]** + **[SCRIPT-BUILD]** | Channels are the pack door's `$channel` entry (already handled by the door); the synthetic preview images must first be BUILT script-side. |
| C3 | **Ordered sink** emission (`result_order`, wire `$seq`) | qa_result_order | **[EXPOSE-SCRIPT]** + **UNSCHEDULED U3** | The Record path's dispatch stamps `$seq` INTO the staged record (COW-when-shared). A sealed pack is immutable — ordering metadata must ride the door args/event instead. The expose pack door already carries its own `seq`; the ordered-STAGING analogue is unowned and must be specified in (or alongside) the [EXPOSE-SCRIPT] design. |
| C4 | **Custom sink instances** fed per-frame from the script | qa_sink_shared_doc (count_sink) | **[USE-DOOR]** (feed leg) | The `$seq` COW double-stamp regression itself is Record-machinery under test — it RETIRES at the cut: sealed single-owner packs make the shared-doc double-stamp class unrepresentable (doc 10 §safety). |
| C5 | data_output config-surface sink | data_output README, image_sources | **N/A by design** | No process() override, no data plane (doc 10 footnote, verified 2026-07-03). |

### D. Script-BUILT payloads

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| D1 | Script constructs **derived images/values** and pushes them | object_count_puzzle (mask), defect_detection.cpp (6 previews), preview_sink_demo | **[SCRIPT-BUILD]** (+ C1 to surface) | The host builder ABI is complete (xi_pack_v1 builder_new/add_*/seal/emit); scripts have no blessed surface to reach it (ScriptPack is read-only; `iface()` escape hatch requires an arrived pack and is not a taught pattern). |
| D2 | **Nested / grouped results** (per-ROI record arrays) | record_demo.cpp (`push("items", …)`), io_stress (nested typed) | **[SCRIPT-BUILD]** + **UNSCHEDULED U4** | Nested data rides as ONE canonical-mp entry (`add_mp`) — building it script-side needs a script-reachable canonical msgpack writer (xi_mp Writer or generated builders). Flagged so the [SCRIPT-BUILD] sibling scopes it explicitly; if in their scope, U4 collapses into that branch. |

### E. Buffering / replay / persistence

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| E1 | **In-memory capture ring + hot-param replay** | buffer_replay_demo (cache), cache README, image_sources (cached_image_source) | **[USE-DOOR]** (capture leg) | cache's pack capture door + ZERO-COPY replay re-emit (same handle, fresh trigger id) + eviction/teardown release are proven against the real DLL in `plugins/cache/tests/test_cache_pack.cpp`. The replayed pack reaches the script via `t.pack()` like any emit. Only the script→cache capture call is missing. |
| E2 | **Persist results to disk** | record_save README, data-out guides | **[USE-DOOR]** (feed leg) | record_save's pack door writes canonical XEX1-v2, byte-identical to expose's wire dump (memory≈wire≈disk), proven in `plugins/record_save_pack_test.cpp`. |
| E3 | **Replay FROM disk** (load a dump back into the pipeline) | record_save README (xex1_pack_load) | **[P3]** | Untrusted-disk load-back through ingress already rebuilds an identical pack in `record_save_pack_test.cpp`; the replay-SOURCE path + migration note for old replay files is Gate P3 (doc 10), scheduled, not started. |

### F. Config-plane patterns over data-plane plugins

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| F1 | **Config swap** (prepare/commit) under live traffic | config_swap_probe README | **[USE-DOOR]** | Record-vs-pack door parity of the probe proven in `plugins/config_swap_probe/tests/test_config_swap_probe_pack.cpp`; the observation surface (get_status) is control-plane JSON and unaffected. |
| F2 | **Retune-and-rerun** via exchange (hue_tune, golden_defect Param) | hue_tune, golden_defect, buffer_replay_demo | **N/A** (control) | exchange()/Params are control plane. The rerun's DATA leg is B2/E1 and inherits their blockers. |

### G. Verdict / status / state

| # | Pattern | Taught by | Status | Evidence / blocker detail |
|---|---|---|---|---|
| G1 | **Per-run verdicts** incl. ok/ng/na/crash classes | qa_run_result, defect_detection.cpp | **GREEN** (orthogonal) | The verdict plane is not a data currency; all three pack QA examples verdict while holding a pack. `qa_pack_stereo`/`qa_pack_walk` use it as the ONLY observability channel — proof it suffices pack-only. |
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

## Scorecard

Counting the 30 pattern rows (A1–A9, B1–B4, C1–C5, D1–D2, E1–E3, F1–F2, G1–G4;
§H is the N/A block):

| Status | Rows | Count |
|---|---|---|
| **GREEN today (pack-only, evidenced)** | A1–A9, G1 | **10** |
| **Flips on [USE-DOOR]** | B1, B2, C4, E1, E2, F1 (+ B3, B4 partially) | **6 (+2 partial)** |
| **Flips on [EXPOSE-SCRIPT]** | C1, C2 (also D-gated), C3 (needs U3 answered) | **3** |
| **Flips on [SCRIPT-BUILD]** | D1, D2 (needs U4 answered), C2 (shared) | **2** |
| **Gate P3 (scheduled)** | E3 | **1** |
| **Unscheduled blocker involved** | B3, B4 (U1), C3 (U3), D2 (U4), G3 (U2) | **5** |
| **N/A (control plane / by design)** | C5, F2, G2, G4 + the §H block | **4 rows + H** |

Bottom line for Gate P2: **the read half of script parity is done and
QA-gated** (everything a script needs to CONSUME packs works in the live
service today). The write half is exactly the three sibling branches — plus
four named semantic gaps (U1–U4) that no branch owns. **P2 cannot be declared
green until B/C/D rows flip AND U1/U3/U4 have owners or explicit
won't-need decisions.**

## Unscheduled blockers (the wave-2 planning input)

- **U1 — Pack-plane NA / provenance / typed-IO script semantics.** No pack
  equivalent of `Record::na()/is_na()/na_reason()` (NA propagation through
  chains), `$prov`/`prov_of()`, `set_src()/src()` (provenance), and no
  `_io`-style typed build/extract helpers for packs beyond doc 10's codegen
  gap #2 (keys-only plugins). Plugin-side `$fault` packs exist and are a good
  seed; the script-side read/propagate contract is unowned. Affects:
  fixturing_demo, io_stress, graph_demo, and the error path of every chained
  example.
- **U2 — `xi::state()` post-Record shape.** Returns `xi::Record&`
  (xi_state.hpp); DocRegistry/COW die at the cut. Keep-as-JSON vs move-to-pack
  is nobody's decision yet. Affects: blob_tracker, trend_monitor,
  hot_reload_run2 (+ `t.meta()`'s Record return, same family).
- **U3 — Ordered-sink semantics on the pack plane.** Dispatch stamps `$seq`
  into staged Records; sealed packs are immutable, so ordering metadata must
  ride the door args/event. Must be specified in or alongside the
  [EXPOSE-SCRIPT] design; today unowned. Affects: qa_result_order's pattern.
- **U4 — Script-reachable canonical-mp writer for nested entries.** Nested/
  grouped results are one `Mp` entry; building one script-side needs an
  xi::mp-writer surface (or generated builders). Belongs in [SCRIPT-BUILD]'s
  scope — flagged so that branch either includes it or this becomes a task.

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
