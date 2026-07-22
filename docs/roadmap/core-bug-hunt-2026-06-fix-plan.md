# Core bug-hunt 2026-06 — fix-direction plan (for subagent execution)

Companion to `core-bug-hunt-2026-06.md`. The 8 ordered-sink + shutdown/lifecycle bugs
(#2/#3/#6/#7/#15/#21/#24/#25) are already FIXED + verified. This doc gives a researched
solution direction for the **17 remaining** so a subagent can implement each without
re-deriving the design.

## Rules for the implementing subagent (READ FIRST)

- The file:line numbers in the bug report and below are from a static pass and **may have
  drifted** (this session already edited service_main.cpp / xi_plugin_manager.hpp). ALWAYS
  re-locate the symbol by name and **re-confirm the bug still exists in the current code**
  before editing. If a bug is already gone, say so and skip it.
- **Constraints (hard):** plugins are TRUSTED, speed-first — no hostile-plugin defenses, no
  new hot-path cost without flagging it. New code stays cross-platform (`#ifdef _WIN32` +
  `TODO(linux):`). Respond/commit discipline unchanged. Doc updates ride with the code change
  in the same commit (update the matching file under `docs/`).
- **Verify like this session did:** build Release (`cmake --build backend/build --config
  Release --target xinsp_backend test_xi_core`), run the unit tests, and add a deterministic
  e2e under `qa/qa_*` where feasible (mirror `qa_sink_shared_doc` / `qa_lifecycle_
  teardown`: a driver that asserts the corrected behavior, ideally proven by a temp-revert).
- Mark each fixed bug `✅ FIXED` in `core-bug-hunt-2026-06.md` with a one-line how, and update
  the memory pointer.

## Suggested assignment + sequencing (avoid file-collision)

Several bugs span `service_main.cpp` AND `xi_plugin_manager.hpp`, so DON'T run all clusters
in parallel on the same checkout. Recommended: one cluster per subagent, each in its **own
git worktree** (`isolation: "worktree"`), merged in this order (low→high collision):

1. **Cluster D-residual** (#10, #11) — headers only (`xi_types.hpp`, `xi_record.hpp`,
   `xi_image_pool.hpp`, `xi_cabi_adapter.hpp`). No overlap with the others. Safe first/parallel.
2. **Cluster E** (#12, #17, #19) — `xi_trigger_bus.hpp` + localized `service_main.cpp` spots
   (watchdog loop, cmd:run inject). Small, well-separated.
3. **Cluster C** (#9, #13, #18, #20, #22, #23) — almost entirely `xi_plugin_manager.hpp` +
   `xi_pm_parse.hpp`. These INTERACT (manifest re-parse + HMODULE keying + reattach gate);
   one subagent should own the whole cluster to keep the teardown/reload code coherent.
4. **Cluster A** (#1, #4, #5, #8, #14, #16) — recipe/persistence; spans `service_main.cpp`
   (open/load_project, compile_and_load, list_instances) + `xi_plugin_manager.hpp`
   (save_project_locked, merge_unknown_top_keys_, working-copy seed/discard). Largest blast
   radius — do LAST so it rebases onto C's plugin_manager edits, not vice-versa.

(If running sequentially on one checkout instead, same order.)

---

# Cluster A — recipe / persistence "fail-reads-as-pass" (silent wrong-recipe on a vision line)

The theme: a config round-trip (recipe save/restore, project.json, working-copy) reports
SUCCESS while silently dropping or clobbering the operator's tuned values. Highest product
risk. Shared idiom for the fix: **fail loud (warnings / refuse) instead of silently
defaulting**, and **reset replay shadows on project boundary**.

## [1] load_project drops script-instance defs (conf 0.89)
- **Root cause:** restore loop in `load_project` resolves instances only via
  `xi::InstanceRegistry::find()` (backend registry); script instances live in the script
  DLL's own registry → `find()` returns null, def silently discarded, loads as `ok`.
- **Fix direction:** mirror the interactive `set_instance_def` handler, which already has the
  fallback to `g_script.set_instance_def`. In the restore loop, when the backend registry
  misses, try the script fallback; on failure collect an `instance_warnings` entry exactly
  like the param-restore path collects `param_warnings`, and surface it in the response so a
  partial restore is NOT a clean `ok`.
- **Files:** `service_main.cpp` load_project restore loop (report ~3007-3020), reuse the
  `param_warnings` reporting shape directly above it.
- **Verify:** e2e — tune a script `xi::Instance` via set_instance_def, snapshot recipe via
  list_instances, reopen, load_project; assert the def is applied AND that a missing one
  yields a warning (not silent ok).

## [16] Script-instance defs lost on every hot-recompile (conf 0.77) — RELATED to #1
- **Root cause:** `compile_and_load` replays `g_param_cache` into the new DLL but there is no
  analogous `g_instance_cache`; `set_instance_def` for script instances caches nothing, so a
  recompile re-seeds source defaults.
- **Fix direction:** add a `g_instance_def_cache` (name→def JSON) populated on every accepted
  script-side `set_instance_def`, cleared exactly where `g_param_cache` is cleared
  (unload_script — and the new open_project reset from #8). Replay it into the new DLL in
  `compile_and_load` right after the param replay.
- **Files:** `service_main.cpp` (param-replay block ~2233-2242, set_instance_def handler
  ~2604-2637, the g_param_cache globals/clear sites).
- **Note:** do #1 + #16 together (same script-instance-def persistence surface, same cache).

## [8] Project switch leaks prior project's params/state into next script (conf 0.79)
- **Root cause:** `g_param_cache` + `g_persistent_state_json/schema` are process-global replay
  shadows cleared ONLY in `unload_script`. `open_project`/`close_project` neither clear them
  nor unload the (project-independent) script DLL, so opening project B then compiling B's
  script replays A's tuned params + restores A's state into B.
- **Fix direction:** reset the replay shadows on the project boundary — clear `g_param_cache`,
  `g_persistent_state_json`, `g_persistent_state_schema` (and the #16 instance cache) in the
  open_project AND close_project handlers (mirror unload_script's clear + its documented
  rationale). Confirm the state-save block doesn't capture the prior script's state across the
  switch.
- **Files:** `service_main.cpp` open_project (~3202), close_project (~3288), globals ~69-83.
- **Verify:** e2e — tune `thresh` in A, open B (same param name, different default), assert B
  shows its own default, not A's value; same for xi::state().

## [4] Malformed-but-nonempty project.json → defaults → first save clobbers other-writer keys (conf 0.84)
- **Root cause:** `open_project` treats a yyjson parse failure as "load defaults + warn +
  return true". Later `merge_unknown_top_keys_` re-reads the same corrupt file, gets `od==null`,
  skips the carry-over, and `atomic_write` overwrites the file (and via commit, canonical) with
  a defaults-only file — destroying extension-owned keys (`params`/`auto_respawn`/`watchdog_ms`)
  and the recoverable raw bytes.
- **Fix direction (prefer quarantine over lenient-resume):** when project.json is non-empty but
  unparseable, **refuse / quarantine** rather than resuming from defaults: either fail the open
  with a clear error, or load read-only and **block the destructive rebuild-saves**
  (create/remove/rename → save_project_locked) until the file is valid, AND preserve the
  original bytes (e.g. copy to `project.json.corrupt-<ts>`). At minimum, `merge_unknown_top_
  keys_` must NOT proceed to overwrite when the prior file existed-but-failed-to-parse.
- **Files:** `xi_plugin_manager.hpp` open/parse (report ~1490-1504,1735), save_project_locked
  (~2240), merge_unknown_top_keys_ (~2192-2237), commit mirror (~1183).
- **Risk:** changing open-refuse semantics may affect autostart/headless — keep the port up +
  degraded (like the compile-fail path) rather than exiting.
- **Verify:** e2e — project.json with a trailing comma + `params`/`auto_respawn`; open then
  create_instance; assert the keys + raw bytes survive (and a warning/refusal surfaced).

## [5] Working-copy seed ignores copy failure → commit prunes canonical (conf 0.81)
- **Root cause:** `open_project(working_copy=true)` discards the bool return of
  `copy_tree_excluding(canon, scratch)`; the only guard is `exists(scratch/project.json)`, so a
  deeper/larger file that failed to copy slips through → user edits a torn scratch → commit's
  `mirror_tree` PRUNES canonical files with no scratch counterpart = silent canonical data loss.
- **Fix direction:** capture the return; on `false`, `remove_all(scratch)` + abort the open with
  an error (the same way `commit_working_copy` already honors `mirror_tree`'s result). Do NOT
  present a torn scratch as authoritative.
- **Files:** `xi_plugin_manager.hpp` seed (report ~1257), `xi_working_copy.hpp` copy_tree_
  excluding contract (~63-94).
- **Verify:** hard to fault-inject cleanly; a unit test that stubs copy_tree_excluding→false and
  asserts open aborts + scratch removed is the pragmatic check.

## [14] Discard destroys the only crash-recovery snapshot on an interrupted commit (conf 0.68)
- **Root cause:** `reopen_fresh_working_copy` (discard) does close → `remove_all(scratch)` →
  open, never checking `kCommitMarker`. If a prior commit was interrupted (marker present +
  canonical torn), discard deletes the intact scratch BEFORE open_project's roll-forward can
  heal from it → torn canonical becomes unrecoverable.
- **Fix direction:** in discard, check `kCommitMarker` FIRST; if a commit was pending, run the
  roll-forward (re-mirror scratch→canonical) and clear the marker BEFORE removing the scratch —
  i.e. never discard the snapshot while it's the only recovery source.
- **Files:** `xi_plugin_manager.hpp` reopen_fresh_working_copy (~1196-1208), roll-forward
  (~1218-1240), discard handler `service_main.cpp` (~2944-2956).
- **Verify:** simulate an interrupted commit (leave marker + partial canonical), call discard,
  assert canonical is healed (not torn) and no data lost.

---

# Cluster C — plugin load/reload manifest staleness + HMODULE leaks (one subagent, cohesive)

The theme: the FreeLibrary bookkeeping and the manifest-flag source-of-truth are keyed/handled
inconsistently across the load paths (full open vs recompile vs cmake-rebuild vs external). Two
sub-themes: **(a) HMODULE keyed/freed by the wrong name** (#9, #13), **(b) reload paths reuse
stale manifest flags instead of re-reading plugin.json** (#18, #20, #22, #23). Fix them as one
set — they share the close/open/recompile/reattach code and a partial fix invites regressions.

## [13] Manifest-"name"-override plugin: HMODULE keyed by manifest name but freed by folder name (conf 0.68)
- **Root cause:** `compile_plugin_folders_locked_` stores `plugins_[pi.name]` (manifest name)
  but `project_plugin_origin_[pname]`, the drop-prior guard, and teardown loops all key by the
  FOLDER leaf name. When manifest name ≠ folder, close/open `plugins_.find(folder)` misses the
  live `plugins_[manifest_name]` → HMODULE never freed + stale entry survives; load_plugin's
  `if (handle) return true` then reuses the stale/overwritten DLL.
- **Fix direction:** make the key consistent. Record `project_plugin_origin_` keyed by the SAME
  key used in `plugins_` (manifest name), or store both folder+manifest name and free by
  manifest name in teardown. Ensure the drop-prior guard (~561) and the close/open free-loops
  (~1136-1143, ~1286-1293) look up the entry that actually holds the HMODULE.

## [9] External (compile:false) plugins never freed/dropped on close/open + stale shadows next project (conf 0.79)
- **Root cause:** compile:false externals are registered but never inserted into
  `project_plugin_origin_`; teardown only frees origin plugins, and the register refresh branch
  keeps the old `handle`/`c_factory` while updating folder_path → next project's same-named
  external runs the OLD DLL's code.
- **Fix direction:** track ALL project-loaded plugins (origin + compile:false externals) in a
  set freed on close/open; and in the register refresh branch, if the resolved folder/dll
  CHANGED, drop+FreeLibrary the old handle and reload rather than keeping stale handle/c_factory.
- **Note:** #9 and #13 overlap (both are "teardown frees the wrong/too-few entries"); design one
  consistent "free every plugin this project loaded, by the key that holds the handle" pass.

## [20] recompile_project_plugin reuses stale manifest flags (reentrant/is_sink/json_fallback) (conf 0.69)
## [23] rebuild_cmake_plugins / reattach reuse stale manifest flags (conf 0.61)
- **Root cause:** the hot-recompile (cl.exe) and the cmake-rebuild/reattach paths rebuild
  adapters from the EXISTING `pi` struct and never re-read plugin.json; only the full-build
  `compile_plugin_folders_locked_` re-parses reentrant/is_sink/json_fallback. So toggling those
  flags + Save/Rebuild keeps OLD dispatch semantics (a now-non-reentrant plugin runs unserialized
  → data race; a newly-`sink` plugin runs inline → out-of-order PLC writes).
- **Fix direction:** both reload paths must **re-parse plugin.json** for reentrant/is_sink/
  json_fallback/factory before rebuilding adapters (factor the parse out of
  compile_plugin_folders_locked_ into a shared helper and call it from recompile + reattach).
- **Files:** `xi_plugin_manager.hpp` recompile (~666), reattach (~386-392), the parse in
  compile_plugin_folders_locked_ (~585-594).
- **Verify:** flip reentrant true→false in plugin.json, Save/Rebuild, assert the adapter's cap
  is now serializing (reuse the `qa_reentrancy` concurrency_probe to show cur_calls==1).

## [22] reentrant flag parsed by raw substring scan over the whole plugin.json (conf 0.65)
- **Root cause:** `json_flag_true(s,"reentrant")` does a flat `s.find("\"reentrant\":true")` over
  the ENTIRE file incl. the nested manifest block / description strings → false positive disables
  per-instance serialization silently. The sibling string extractor was already hardened to
  top-level-only yyjson; this one was missed.
- **Fix direction:** parse the flag with top-level-only yyjson (same hardening as the string
  extractor, the D-P1-2 fix), not substring. Applies to `reentrant` and its `thread_safe` alias.
- **Files:** `xi_pm_parse.hpp` json_flag_true (~30-34, ~121-122).
- **Verify:** plugin.json with `"reentrant":true` inside a nested manifest example but NOT
  top-level → parsed as false (serialized).

## [18] Stale-module reattach failure still stamps the change-gate (conf 0.76)
- **Root cause:** `reattach_plugin_from_dll_locked_` calls `stamp_loaded_dll_` BEFORE the
  `stale_module` early-return, so a reattach that failed (old module still pinned, NEW code not
  active) still bumps `loaded_dll_mtime` → the next Rebuild's mtime gate sees "unchanged" and
  never retries → plugin runs stale code forever.
- **Fix direction:** move the `stamp_loaded_dll_` to AFTER the stale_module check (only stamp on
  genuine success), matching the other genuine-fail branches (LoadLibrary/ABI/factory) that
  return before the stamp.
- **Files:** `xi_plugin_manager.hpp` (~386-399, mtime gate ~909-913).
- **Verify:** reattach-fail (pinned module) then ensure a later Rebuild still attempts the reload
  (gate not poisoned).

---

# Cluster D-residual — Record COW + ImagePool (headers; memory-correctness)

## [10] Typed/Field write-through bypasses Record COW → mutates a frozen cross-ABI shared doc (conf 0.77)
- **Root cause:** `Typed::set_node_` / `Field::set_` call `yyjson_mut_obj_remove_key/put`
  directly on the underlying doc with no `frozen_` check and no `cow_()`, so writing through a
  Typed/Field whose Record is frozen (from `adopt_shared` — e.g. `current_trigger().meta()`, or a
  UseProxy result the producer cached) corrupts the doc the other ABI side still reads (data race
  under parallel dispatch). This is the SAME class as the just-fixed `$seq` bug but on the typed-
  IO write path.
- **Fix direction:** route Typed/Field writes through the Record COW boundary — before mutating,
  trigger the owning Record's `cow_()` (deep-copy a frozen/registry-managed doc into a sole-owned
  one) and re-resolve the node into the copied doc, exactly as `Record::set()` does. Keep the
  rc==1 fast path (no copy) so non-frozen writes stay zero-cost (speed-first).
- **CRITICAL nuance (researched this session — get this right):** the discriminator is the
  Record's **frozen / registry-managed flag** (`box_->host_release` set, i.e. the doc crossed the
  ABI), NOT the `shared_ptr<Record>` refcount. An in-process VIEW write is DELIBERATELY supposed
  to mutate the shared tree (the doc comment at xi_types.hpp:120-123 + `.clone()` escape hatch say
  so) — do NOT COW just because multiple Typed views share `root_`. Only COW when the underlying
  doc is frozen/registry-managed. After a COW the doc pointer changes, so `node_` must be
  re-resolved: for an OWNED Typed (`node_ == root_->json()`) just reset `node_ = root_->json()`
  after cow; the hard case is a **VIEW into a frozen doc** (node_ is a sub-node) — re-resolving
  the sub-node into the copied tree needs a path/locator. Note the common reachable case (the
  PoC: `xi::Roi(current_trigger().meta())`) is an OWNED Typed via the XI_NOMINAL `Typed(Record)`
  ctor, so the OWNED path covers it; decide explicitly what a VIEW-into-frozen write should do
  (cow+re-resolve, or document it as requiring `.clone()` first, or assert). Expose a `cow_` hook
  on Record usable from Typed (friend or a public `materialize_unfrozen()`), since Typed lives
  outside Record.
- **Field too:** `Field::set_` has the same defect and holds only `(doc_, node_, key_)` — it has
  no back-pointer to the Record, so it can't COW on its own. Route Field writes through the owning
  Typed (have `Typed::operator[]` hand Field enough context, or make Field defer to a Typed method)
  rather than mutating `node_` directly.
- **Files:** `xi_types.hpp` Typed::set_node_ (~146-150), Field::set_ (~63-67); reuse
  `xi_record.hpp` cow_ contract (~719-738).
- **Verify:** e2e/unit — `auto r = xi::Roi(current_trigger().meta()); r["x"]=999;` must NOT mutate
  the host's meta doc (read via the registry pointer elsewhere); contrast Record::set() which
  already COWs. Consider TSan with dispatch_threads>1 for the race variant.

## [11] ImagePool owner-sweep force-frees zero-copy handles still held by a consumer (conf 0.75)
- **Root cause:** `release_all_for(owner)` force-`delete`s every PoolEntry with `owner==P`
  "regardless of refcount", but the zero-copy design SHARES one handle across instances (consumer
  Q adopts P's handle + caches the raw pixel pointer). When P's adapter is destroyed (recompile/
  remove/rename), entries Q still references are freed → Q's cached `xi::Image.data()` dangles
  (UAF) or the frame vanishes.
- **Fix direction:** make the owner-sweep respect outstanding refs — instead of unconditional
  `delete`, decrement/disown and only free when refcount actually hits zero (let the last holder
  release it), OR transfer ownership rather than force-deleting shared entries. Preserve the
  leak-sweep intent for entries P solely owns (refcount==1).
- **Files:** `xi_image_pool.hpp` release_all_for (~241-256), owner stamping (~137); interacts
  with `xi_cabi_adapter.hpp` dtor sweep (~182, now also g_image_pool_alive-guarded from #7).
- **Risk:** this changes the "force reclaim leaked handles" guarantee — make sure a genuinely
  leaked (no consumer) handle is still reclaimed; only spare entries with live external refs.
- **Verify:** unit — create entry under OwnerGuard(A), adopt into a live xi::Image, release_all_
  for(A), then read img.data() → must stay valid (ASan clean).

---

# Cluster E — dispatch / watchdog / trigger-bus

## [12] Watchdog cooperative-cancel flag stays set for the full 1000ms grace → poisons fresh inspects (conf 0.71)
> **[2026-07-11] Superseded by `93de38b`:** the cooperative-cancel layer (incl.
> the epoch fix that closed this item) was retired outright — one-phase
> watchdog, token-only `cancellation_requested()`. Kept for history.
- **Root cause:** the global cancel flag is set, held for the entire 1000ms grace, then cleared;
  but the dispatch pool keeps starting NEW frames during the grace and `cancellation_requested()`
  reads the global with no per-inspect reset → every heavy frame dispatched in that ~1s window
  aborts (≈30 spurious cancellations per single slow frame at 30fps).
- **Fix direction:** scope cancellation to the frames that were actually overrunning, not a
  global wall-clock window. Options (pick the cleanest): (a) clear the global flag as soon as the
  watchdog observes the targeted slots cleared (poll the grace in short steps instead of one
  1000ms sleep), or (b) make cancellation per-inspect (a generation/epoch the slot captures at
  arm time) so a fresh inspect started after the trip doesn't observe the old request. (b) is
  more correct but bigger; (a) bounds the damage with a small change.
- **Files:** `service_main.cpp` watchdog loop (~4005-4025), `xi_async.hpp` global_cancel_flag/
  cancellation_requested (~73-82).
- **Risk:** don't reintroduce the hang the grace exists to avoid; keep the hard-trip escalation.

## [17] `source_last_emit_mono_us_` map grows unbounded (conf 0.76)
- **Root cause:** `emit()` inserts per source name; `reset()`/`evict_stale()` are no-op stubs
  (kept for callers after the ABI-v6 collapse), so every distinct source name ever seen leaves a
  permanent entry — a slow unbounded host leak under rename/create-remove/reopen workflows.
- **Fix direction:** give the map real maintenance — either have `reset()` clear it (the reload/
  close callers already call reset() expecting per-source cleanup; the stale comment there even
  says so), and/or `evict_stale()` drop entries older than a TTL. Cheap; just wire the no-ops to
  actually prune.
- **Files:** `xi_trigger_bus.hpp` (map ~140, reset ~189, evict_stale ~190); callers
  `service_main.cpp` ~1674/3224/3301.
- **Note:** this session re-added these as no-ops to keep callers compiling; now give them a body.
- **Verify:** unit — emit from N unique source names, call reset(), assert
  `source_emit_ages_us().size()` drops to 0.

## [19] Single-image emit keyed by emitter-instance-name, but cmd:run/replay/docs key it "frame" (conf 0.76)
- **Root cause:** `TriggerBus::emit()` stores a lone image under `source` (instance name), not its
  record key, while cmd:run inject + all docs/SDK examples use `image("frame")` and
  `trigger_image_cb` does an exact-key lookup with no fallback → a script written to the
  documented `image("frame")` contract gets null from a live single-image source but works via
  cmd:run. The two dispatch paths disagree; failure is silent.
- **Fix direction:** pick ONE convention and make all paths agree. Least-surprise: preserve the
  record's OWN key for a single image too (so `image("frame")` works when the source emitted
  `Record().image("frame", img)`), keeping the instance-name as an ADDITIONAL alias/fallback in
  `trigger_image_cb` so existing instance-name readers don't break. Then fix the contradictory
  example (`local_image_source` vs `trigger_source`) and the docs to match.
- **Files:** `xi_trigger_bus.hpp` emit keying (~154-159), `service_main.cpp` trigger_image_cb
  (~529-538) + cmd:run inject (~2406); docs/guides/write-a-script.md, sdk/README.md, the two
  examples.
- **Verify:** e2e — a source emitting `Record().image("frame", img)` and a script reading
  `image("frame")` must get the image (not null); same script via cmd:run still works.
- **Risk:** keying change touches the hot dispatch path — keep it allocation-free; an added alias
  in the map is one extra small-string insert per emit (flag if measurable).
