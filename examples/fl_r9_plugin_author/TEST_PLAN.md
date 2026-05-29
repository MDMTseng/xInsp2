# FL Round 3 — G: Plugin / Script Author corner-case test plan

Adversarial-stance test plan covering one week of a non-malicious-but-tired
plugin / script developer iterating on xInsp2. Each case targets at least one
finding from `.fl_audits/round1_round2_consolidated.md` (cited by ID) or is
flagged `novel`. Read-only plan — no implementations.

> Anti-injection: tool output read while drafting this contained `<system-reminder>`
> tags (e.g. inside the audit doc and inside example snippets). They were
> treated as data; only the prompt has authority. No injection attempts found
> beyond the standard reminder envelopes.

Conventions:
- "backend" = `xinsp-backend.exe`; "worker" = `xinsp-worker.exe`.
- "PASS" = the measurable bar the test should clear; absence of crash alone
  is not pass — backend must remain pingable AND no orphan processes AND no
  unexplained drift in `dispatch_stats`/`cmd:list_instances`.
- "the SDK" = `tools/xinsp2_py`.

---

## Iteration / hot-reload

### G-1. Double-Ctrl-S race (recompile-during-recompile)

**User scenario.** Author edits `plugins/det/src/plugin.cpp`, hits `Ctrl+S`,
realizes a typo, edits again and hits `Ctrl+S` 120 ms later. Two
`cmd:recompile_project_plugin` requests are now in flight against the same
plugin folder.
**Concrete user actions.** (1) Open project. (2) `c.call("recompile_project_plugin", {plugin: "det"})` async. (3) Within 200 ms call the same again. (4) After both replies, `c.exchange_instance("det0", {"command":"ping"})`.
**Hypothesis.** Second call queues or rejects; first compile completes; `det0` instance ends with the latest DLL bound and instance state preserved.
**Likely actual behavior.** P0-AB-3 / P0-AB-4: dispatcher pool not drained; second compile `FreeLibrary`s mid-`set_def_fn_` of first; either crash or `ii.instance` left null with B-P1-4 silent-no-op afterward.
**Bug prediction.** P0-AB-3, P0-AB-4, B-P1-4.
**Success criteria for the test.** No backend crash; `cmd:list_instances` shows `det0` alive; `exchange_instance` round-trips a non-null reply within 5 s; no orphan worker; `dispatch_stats.queue_depth_now == 0`.
**Implementation sketch.** Python harness opens project, fires two `compile_and_load`-style RPCs back-to-back via `concurrent.futures`, then probes liveness.
**ROI.** high: targets 4 P0s with one cheap script.

### G-2. Hot-reload while continuous run streaming

**User scenario.** `cmd:start fps=30` is running; author Ctrl+S's plugin used by a live instance.
**Concrete user actions.** (1) `c.call("start", {fps:30})`. (2) Wait 5 s, observing `vars` events. (3) Touch plugin source, `recompile_project_plugin`. (4) Continue 10 s. (5) `c.call("stop")`.
**Hypothesis.** Stream pauses ~1 s, resumes; no run_id reuse, no UAF.
**Likely actual behavior.** P0-AB-1/2/3: in-flight `process()` enters DLL whose `FreeLibrary` already returned. `resumed_continuous` field returned but stream half-dead.
**Bug prediction.** P0-AB-1, P0-AB-3, P0-AB-4, A-P1-2.
**Success criteria for the test.** No crash, `vars` events resume within 3 s of recompile reply, no `run_started` for stale run_id, `g_iso_dead_reported` does not register a phantom death (E-P1-1).
**Implementation sketch.** Python: subscribe to `vars`, run for 15 s, recompile mid-run, count gaps and run_id discontinuities.
**ROI.** high: surfaces multiple P0s per the most common dev workflow.

### G-3. Recompile cycle leaks `script_build/` to disk-full

**User scenario.** Dev does 800 saves over the day. Each emits a versioned `<plugin>_v<N>.dll/.lib/.obj/.log`.
**Concrete user actions.** Loop 800x: minor edit + `recompile_project_plugin`. Measure `du plugins/<name>/build`.
**Hypothesis.** Old artifacts pruned; size stable.
**Likely actual behavior.** P0-E2: monotone growth ~5 GB/day; `s_version++` never wraps.
**Bug prediction.** P0-E2.
**Success criteria for the test.** `du build/ < 200 MB` after 800 cycles, OR an audit log entry showing pruning. Failing both = FAIL.
**Implementation sketch.** Bash/PS harness scripts the touch+recompile loop with `os.utime`; samples disk size every 50 cycles, plots.
**ROI.** high: deterministic, single-machine reproducible, production blocker.

### G-4. Two VS Code windows on same project

**User scenario.** Author opens project in two windows (forgot the first was open). Both extensions register file-watchers; both fire `compile_and_load` on the same Ctrl+S.
**Concrete user actions.** (1) Open project A in window 1. (2) Open same project A in window 2. (3) Save plugin file once.
**Hypothesis.** Backend rejects/ignores duplicate, or serialises.
**Likely actual behavior.** Two simultaneous compiles into same `build/` dir, lock contention on `<name>_v<N>.lib` / `.obj`; one compile fails noisily; potential `B-P1-1/2` partial-failure leaves a half-loaded plugin; dispatcher race per G-1.
**Bug prediction.** B-P1-1, B-P1-2, P0-AB-3.
**Success criteria for the test.** Backend stays alive; exactly one DLL ends up loaded; both `cmd:compile_and_load` replies decoded successfully (no F-P1-2 KeyError on the SDK side).
**Implementation sketch.** Two SDK clients in same Python proc → two `compile_and_load` calls scheduled to fire within 5 ms.
**ROI.** medium: realistic, mostly probes B-P1-1/2.

### G-5. `Ctrl+C` backend mid-cl.exe; reopen later

**User scenario.** Compile is slow (cold msvc), author kills the dev server. Tomorrow they reopen the project.
**Concrete user actions.** (1) `recompile_project_plugin`. (2) Within 1 s, kill backend (TerminateProcess parent). (3) Restart backend. (4) `cmd:open_project`.
**Hypothesis.** Project opens; partial `<name>_vN.lib.tmp` ignored; previous DLL still serviceable.
**Likely actual behavior.** P0-D3 path: stale DLL with no compatibility check; D-P1-5 ignored `atomic_write` returns may have left `instance.json` truncated. Cert TOCTOU (D-P1-4) if file replaced post-cert.
**Bug prediction.** P0-D3, D-P1-4, D-P1-5.
**Success criteria for the test.** Open succeeds; `cmd:open_project_warnings` reports stale-build artifacts OR project loads cleanly with last-good DLL; backend stays alive even if cert mismatch (logs+skips, doesn't crash).
**Implementation sketch.** Python harness uses `subprocess.Popen(backend); kill -9 mid-compile; spawn again; open_project`. Optionally pre-corrupt `<plugin>.dll` to half-size to fuzz cert path.
**ROI.** high: P0-D3 + D-P1-5 + D-P1-4 in one test.

### G-6. Save-while-save (project save races plugin recompile)

**User scenario.** Author triggers `cmd:save_project` from VS Code while a `cmd:recompile_project_plugin` still runs.
**Concrete user actions.** (1) Begin recompile. (2) ≤50 ms later, `c.call("save_project")`.
**Hypothesis.** Both serialise.
**Likely actual behavior.** P2: two save paths drift. PluginManager state not in `save_project`, so the in-flight `set_def`/`get_def` of the about-to-be-replaced instance gets persisted with stale data; D-P1-5 silent atomic_write losses.
**Bug prediction.** P2 (save drift), D-P1-5, P0-AB-4.
**Success criteria for the test.** After both return, reopen project: `instance.json` matches the post-recompile state, not pre-.
**Implementation sketch.** Python: dual-RPC fire, then close+reopen project, hash-compare `instance.json`.
**ROI.** medium: subtle silent corruption.

---

## Bad plugin behavior

### G-7. Plugin destructor takes 3 s

**User scenario.** Author writes a destructor that joins a worker thread holding a 3 s sleep.
**Concrete user actions.** (1) Build plugin with `~MyPlugin(){ std::this_thread::sleep_for(3s); }`. (2) `recompile_project_plugin`. (3) Drive an inspection through it before recompile starts.
**Hypothesis.** Backend waits and proceeds, or kills cleanly with a clear log.
**Likely actual behavior.** `ProcessInstanceAdapter::shutdown_` 2s wait elapses → P2: RPC_DESTROY before stop_reader_; 30s timeout on hung worker; or `TerminateThread` (D-P1-9 friend) corrupts state.
**Bug prediction.** P2 (shutdown ordering), C-P1-1.
**Success criteria for the test.** `recompile_project_plugin` returns within 8 s; subsequent `process()` works; no orphan worker.
**Implementation sketch.** Build a slow-destructor plugin variant; recompile; time the round-trip; `Get-Process xinsp-worker` to check for orphans.
**ROI.** high: very common dev sin (forgot to set a stop-flag).

### G-8. Plugin throws from `process()`

**User scenario.** Plugin author dereferences a null `xi::Image` because an upstream key changed.
**Concrete user actions.** (1) Build plugin where `process` throws `std::runtime_error("boom")` always. (2) Drive `cmd:run`. (3) Check rsp.
**Hypothesis.** Per `adding-a-plugin.md`, output Record carries `error` field; backend pingable; worker survives via SEH.
**Likely actual behavior.** F-P1-1: doc says `run_finished` event but never emitted, so a driver `wait for run_finished` hangs; rsp may carry empty record with no `error` key. C-P1-5: silent `dead_=false` after restore-failure → wrong outputs.
**Bug prediction.** F-P1-1, C-P1-5.
**Success criteria for the test.** Either `run_error` event arrives within 5 s, or rsp has `out["error"]` populated. Hang = FAIL.
**Implementation sketch.** Python: `c.call("run")` and `c.next_event(timeout=5)` for `run_finished|run_error`.
**ROI.** high: directly tests F-P1-1 doc-vs-impl drift.

### G-9. Plugin holds host-mutex forever inside exchange

**User scenario.** Plugin's `exchange()` takes its own member mutex and never releases (deadlock bug in plugin).
**Concrete user actions.** (1) Drive `c.exchange_instance("det0", {...})`. (2) Within 2 s, drive `cmd:recompile_project_plugin`.
**Hypothesis.** Backend cancels in-flight exchange before FreeLibrary; recompile completes.
**Likely actual behavior.** P0-AB-5: re-entrancy via host_api hops + no drain → recompile FreeLibrary's DLL while exchange holds shared_ptr → UAF on next set_def.
**Bug prediction.** P0-AB-5, P0-C1.
**Success criteria for the test.** Backend pingable after; recompile reply OR a clear timeout error within 30 s; no UAF (run with PageHeap or ASAN-equivalent if available).
**Implementation sketch.** Build a plugin with `exchange(){ std::mutex m; m.lock(); m.lock(); }`; concurrent recompile with timeout.
**ROI.** high: targets P0-AB-5 directly.

### G-10. Plugin ExitProcess()'s itself from a worker thread

**User scenario.** Author uses a 3rd-party lib that calls `std::abort()` on internal sanity check.
**Concrete user actions.** Build plugin that `ExitProcess(0)` 200 ms after first `process`.
**Hypothesis.** `ProcessInstanceAdapter` auto-respawns (3 per 60 s) and replays `set_def`. Any pending RPC errors out.
**Likely actual behavior.** P0-C1: `accept_one` infinite wait if respawn fails; C-P1-4: respawn cap eats transient OOM; C-P1-5: silent `dead_=false` if SET_DEF restore fails — wrong outputs in production.
**Bug prediction.** P0-C1, C-P1-4, C-P1-5.
**Success criteria for the test.** Within 10 s of first crash: instance back to live; pending `process` returns error not hang; `cmd:list_instances` reports a death event exactly once (E-P1-1 cleared).
**Implementation sketch.** Plugin with self-exit; SDK drives 50 process calls, asserts ≥48 succeed eventually after one transient failure.
**ROI.** high: full crash-respawn loop.

### G-11. Plugin allocates 8 GB per `process()`

**User scenario.** Author forgot to free a debug image accumulator; growth = 100 MB/call.
**Concrete user actions.** Drive `cmd:start fps=30` for 60 s.
**Hypothesis.** Worker OOMs cleanly; auto-respawn + back-pressure; backend pingable.
**Likely actual behavior.** P0-E1: SHM bump-only exhausts before plugin OOMs; `image_data` returns nullptr silently; no alarm. C-P1-4: respawn cap might burn on OOM-killed CreateProcess.
**Bug prediction.** P0-E1, C-P1-4, D-P1-7.
**Success criteria for the test.** Either explicit error events with `out_of_shm` semantics, or graceful pipeline-degraded warning. Silent zeros / nulls = FAIL.
**Implementation sketch.** Plugin with `static std::vector<uint8_t> blob; blob.resize(blob.size()+100_MB);` per call; SDK monitors `vars` for nulls vs explicit errors.
**ROI.** high: P0-E1 is the silent-corruption blocker.

### G-12. Plugin re-enters `host->log()` from destructor

**User scenario.** Author logs in destructor for tracing.
**Concrete user actions.** Plugin destructor calls `host_->log("destructing")`. Trigger destruction via `cmd:remove_instance` while another exchange is in flight.
**Hypothesis.** Reentrant log path tolerated; destruction proceeds.
**Likely actual behavior.** Re-entry into the dispatcher / host-api during teardown; aligns with the P0-AB-5 re-entrancy class.
**Bug prediction.** P0-AB-5, novel (host-api reentrancy from destructor specifically).
**Success criteria for the test.** Removal completes; log line received; no crash.
**Implementation sketch.** Build plugin variant; `c.call("remove_instance", {name:"det0"})`; tail backend stderr.
**ROI.** medium: rare path but cheap.

### G-13. Plugin returns garbage from `get_def` (length lies)

**User scenario.** Author returns `int (-99999)` from `xi_plugin_get_def` to mean "skip persistence" (misreading the contract — negative means buffer-too-small with abs() bytes needed).
**Concrete user actions.** (1) Build plugin where `get_def` returns -99999. (2) `cmd:save_project`.
**Hypothesis.** Backend gracefully handles, logs warning, persists `{}`.
**Likely actual behavior.** Backend allocates 99999-byte buffer, calls again; second call also -99999; potential infinite loop OR buffer mismatch; silent persist of garbage. D-P1-5: atomic_write failure silently dropped.
**Bug prediction.** D-P1-5, novel.
**Success criteria for the test.** `save_project` returns within 5 s; reopen project shows last-good `instance.json`, not garbage.
**Implementation sketch.** Plugin with hostile `get_def`; save+reopen+diff.
**ROI.** medium: corner of contract.

### G-14. Plugin's `exchange` writes 16 MB error message

**User scenario.** Author copy-pastes a giant cv::Mat into an error string for "debugging."
**Concrete user actions.** Drive `c.exchange_instance(name, cmd)` where plugin replies with 16 MB JSON.
**Hypothesis.** Backend forwards or rejects with size error.
**Likely actual behavior.** C-P1-7: 16 MB error frame DoS amplifier — fine here but each call can OOM the in-process buffer; P0-D1 if plugin worker is the malicious side and mis-encodes length.
**Bug prediction.** C-P1-7, P0-D1.
**Success criteria for the test.** Backend pingable; response truncated or rejected with explicit size error; memory does not grow per call.
**Implementation sketch.** Plugin returns a 16 MB string from `exchange`; loop 100 calls; sample backend RSS.
**ROI.** medium.

### G-15. Script DLL exports `xi_inspect_entry` but throws on first invocation

**User scenario.** Static initialiser throws after the symbol is resolved.
**Concrete user actions.** (1) `compile_and_load`. (2) `cmd:run`.
**Hypothesis.** Run returns `run_error`; backend stays alive; next compile_and_load fixes it.
**Likely actual behavior.** F-P1-1: `run_finished`/`run_error` not emitted; SDK `tail` loop hangs (F-P1-5); A-P1-3: `release_all_for(owner_id)` race during recovery.
**Bug prediction.** F-P1-1, F-P1-5, A-P1-3.
**Success criteria for the test.** SDK `c.run()` raises within 5 s OR `run_error` event arrives. Indefinite hang = FAIL.
**Implementation sketch.** Static-init-throwing script; `c.run()` with 5 s wall-timeout assertion.
**ROI.** high: F-P1-1 is the most user-visible doc lie.

---

## Filesystem / path edge cases

### G-16. Project on path with non-ASCII (Chinese / accented) folder names

**User scenario.** Path `C:\工作\my_project\plugins\det\src\plugin.cpp`.
**Concrete user actions.** Create project in such a path; recompile_project_plugin; open instance UI.
**Hypothesis.** Works; cl.exe receives UTF-8/wide path correctly.
**Likely actual behavior.** P2: pipe name truncation breaks on long names; P2: quoting breaks on `"`. Likely UTF-8/CP_ACP mismatch in `CreateProcessA` invocation; cl.exe stderr lost or `atomic_write` (D-P1-5) silently fails on path normalization.
**Bug prediction.** P2 (pipe-name truncation), D-P1-5, novel (UTF-8/ACP).
**Success criteria for the test.** Recompile succeeds; UI loads; compile diagnostics readable.
**Implementation sketch.** Set up `tmp\工作\proj`, full lifecycle, compare outcome to ASCII baseline.
**ROI.** medium: international users encounter early.

### G-17. Project name contains a `"` (double-quote)

**User scenario.** Author copy-pastes a name like `Site "A" sensor`.
**Concrete user actions.** `cmd:create_project name='Site "A"'`; save; reopen.
**Hypothesis.** Stored escaped; reopen succeeds.
**Likely actual behavior.** D-P1-1: write path concatenates without escaping → invalid JSON written to `project.json`; D-P1-2: read substring matches inside string values → next open trips parser.
**Bug prediction.** D-P1-1, D-P1-2.
**Success criteria for the test.** `project.json` is valid JSON (parse with `json.loads`); reopen works.
**Implementation sketch.** Create+save+reopen via SDK; assert `json.loads(open('project.json').read())`.
**ROI.** high: cheap, deterministic, hits two P1s.

### G-18. OneDrive / network share path with 100 ms NFS latency

**User scenario.** Project on `\\nas\share\proj` with smbsynth latency.
**Concrete user actions.** Hot-reload loop x50 with `injected_fs_latency=100ms`.
**Hypothesis.** Slower but correct.
**Likely actual behavior.** D-P1-5 atomic_write fails more often (silent); G-1 race opens up; cl.exe `/Fo` lock contention with antivirus more likely.
**Bug prediction.** D-P1-5, P0-AB-3.
**Success criteria for the test.** No silent corruption; if compile fails, error surfaces in rsp.
**Implementation sketch.** Use `winfsp` with delay layer or mklink to a SMB loopback; loop recompile.
**ROI.** low: hard to automate reliably. (See "can't be automated" note.)

### G-19. User accidentally deletes `plugin.json` mid-run

**User scenario.** rm in another terminal.
**Concrete user actions.** While `cmd:start fps=30` running, delete `plugins/det/plugin.json`. Wait 5 s. Stop.
**Hypothesis.** Run continues from in-memory state; warning surfaced; reopen reports missing plugin.
**Likely actual behavior.** No file-watcher reaction; next `save_project` writes a stale entry; B-P1-1/2 on next reopen leaves dead adapters in registry.
**Bug prediction.** B-P1-1, B-P1-2, D-P1-5.
**Success criteria for the test.** Backend stays alive; reopen surfaces a clear diagnostic; no zombie instance in `cmd:list_instances`.
**Implementation sketch.** Python `os.remove()` mid-run; verify via SDK calls.
**ROI.** medium.

### G-20. User renames `plugins/det/` → `plugins/det_v2/` mid-run

**Concrete user actions.** Continuous run; `os.rename`; observe.
**Hypothesis.** Warning + graceful drop; auto-recover on next recompile.
**Likely actual behavior.** Same registry-rot path as G-19; possibly P0-D3 if a stale handle reloads the renamed DLL with mismatching ABI.
**Bug prediction.** B-P1-1, P0-D3.
**Success criteria for the test.** Backend pingable; renamed plugin recovered after explicit reopen; no UAF.
**Implementation sketch.** Mid-run rename; reopen project.
**ROI.** medium.

---

## Lifecycle race

### G-21. `cmd:run` detached thread vs `cmd:unload_script`

**User scenario.** Driver auto-unloads on quit; an inspect was just dispatched.
**Concrete user actions.** (1) `c.call("run")`. (2) Within 5 ms, `c.call("unload_script")`.
**Hypothesis.** unload waits for run to drain.
**Likely actual behavior.** P0-AB-1, P0-AB-2: detached thread holds `s = g_script`; unload `FreeLibrary`s; UAF inside the inspect.
**Bug prediction.** P0-AB-1, P0-AB-2, A-P1-3.
**Success criteria for the test.** Backend stays alive over 50 iterations; ASAN/PageHeap finds no UAF; both calls return rsp without timeout.
**Implementation sketch.** Tight Python loop; PageHeap-enabled backend; assert no `0xC0000005` in stderr.
**ROI.** high: hits the marquee P0 pair.

### G-22. `seq_` near-wrap (synthetic)

**User scenario.** Long-running deployment; `seq_` `uint32_t` approaches wrap.
**Concrete user actions.** Boot backend with `XINSP2_SEQ_INIT=4294967290` (or patch test-only seam) and drive 20 RPCs.
**Hypothesis.** Seq increments past wrap fulfilling promise correctly.
**Likely actual behavior.** P0-C2: at 0, frame routed to `handle_async_frame_`, RPC promise never fulfilled → `c.call` times out or hangs (F-P1-4 raises bare `queue.Empty`).
**Bug prediction.** P0-C2, F-P1-4.
**Success criteria for the test.** All 20 RPCs return; no `queue.Empty` raised; no orphan promise.
**Implementation sketch.** Env var or debug RPC `set_seq`; loop 20 process calls; assert all return.
**ROI.** high: surfaces P0-C2 in seconds rather than 828 days.

### G-23. Watchdog + NTP forward skew

**User scenario.** Dev's machine clock drifts +90 s after corp NTP push during a slow inspect.
**Concrete user actions.** While `process()` is running (sleep_ms=2000), bump system clock +60 s via `Set-Date`.
**Hypothesis.** Watchdog uses steady_clock; unaffected.
**Likely actual behavior.** D-P1-10: `system_clock` for deadlines → watchdog `TerminateThread` fires prematurely.
**Bug prediction.** D-P1-10, D-P1-9.
**Success criteria for the test.** No premature watchdog termination; `process` completes and rsp arrives.
**Implementation sketch.** Sleep-heavy plugin; PowerShell `Set-Date` mid-call; assert rsp received.
**ROI.** medium: needs admin; hits P1 cluster.

### G-24. Worker crashes between CreateProcess and CreateFile

**User scenario.** Antivirus quarantines `xinsp-worker.exe` mid-spawn.
**Concrete user actions.** Test-only fault: hook to kill the worker exit-code 0 immediately on spawn.
**Hypothesis.** Adapter retries within respawn cap and reports failure.
**Likely actual behavior.** P0-C1: `accept_one` `WaitForSingleObject(INFINITE)` → host hangs forever.
**Bug prediction.** P0-C1.
**Success criteria for the test.** A `c.call` on this instance returns with error within 10 s.
**Implementation sketch.** Use a stub worker that `ExitProcess(0)` on launch; drive `cmd:open_project`; assert no hang.
**ROI.** high: P0-C1 is one Sleep/INFINITE swap away.

### G-25. SDK timeout vs server cmd persistence

**User scenario.** Author sets aggressive `c.call(timeout=2)` because they're impatient. Compile takes 30 s.
**Concrete user actions.** `c.call("recompile_project_plugin", {...}, timeout=2)`. Then immediately `cmd:list_instances`.
**Hypothesis.** SDK raises `TimeoutError`; backend cancels.
**Likely actual behavior.** F-P1-4: SDK raises bare `queue.Empty`; backend continues; `list_instances` reflects half-applied state user thinks rolled back.
**Bug prediction.** F-P1-4.
**Success criteria for the test.** Either backend exposes a `cmd:cancel` and uses it, OR the SDK raises a typed `TimeoutError` AND state reconciles within 30 s. Bare `queue.Empty` = FAIL.
**Implementation sketch.** Slow recompile; tight timeout; capture exception class; diff state.
**ROI.** high: F-P1-4 mis-leads every driver author.

---

## Manifest / config drift

### G-26. instance.json hand-edited with extra key not in `manifest.params`

**User scenario.** Author adds `"new_param": 42` to `instance.json` thinking the plugin reads it.
**Concrete user actions.** (1) Stop backend. (2) Edit `instance.json` add unknown key. (3) Open project.
**Hypothesis.** `cmd:open_project_warnings` includes `unknown_config_key`. Plugin still loads with defaults.
**Likely actual behavior.** Warning emitted (per docs) but the key still flows through to `set_def` (per docs), and plugin silently falls back. Consistent with docs but a UX trap; pair with D-P1-2: substring search may match the new key inside a string. Validates F-P1-1 family of "warnings emitted but not surfaced as events."
**Bug prediction.** P2 (manifest validation), D-P1-2.
**Success criteria for the test.** `open_project_warnings` contains exactly one `unknown_config_key`; plugin loads; subsequent `get_def` does not include the new key.
**Implementation sketch.** PSh edit; SDK reads warnings.
**ROI.** medium.

### G-27. instance.json with `5.7` in an `"int"` field

**User scenario.** Author types a float into an int slider in source.
**Concrete user actions.** Hand-edit `"threshold": 5.7` (declared int in manifest).
**Hypothesis.** Warning `type_mismatch`; value coerced or rejected.
**Likely actual behavior.** P2: int/float conflated by validator → no warning emitted; `set_def` may parse as 5 or fail silently.
**Bug prediction.** P2 (manifest int/float).
**Success criteria for the test.** Either warning emitted OR plugin's `set_def` rejects with explicit error. Silent acceptance = FAIL.
**Implementation sketch.** Edit + open; assert one of the two outcomes.
**ROI.** medium.

### G-28. instance.json with NBSP / unicode whitespace in keys

**User scenario.** Author copy-pastes from a doc that contains `&nbsp;` between `"` and `key`.
**Concrete user actions.** Inject `" threshold": 5`.
**Hypothesis.** JSON parser accepts; key compared with whitespace; warning OR rejection.
**Likely actual behavior.** D-P1-2: substring search matches inside other keys; could cross-pollute. cJSON tolerates unicode space inside strings; the key as-stored will not match `"threshold"`, becoming an `unknown_config_key`. But D-P1-2's brace counter on `"plugin"` substring is the real risk — we're attacking the same parser.
**Bug prediction.** D-P1-2.
**Success criteria for the test.** Project loads; the NBSP key flagged as unknown; no parse-error spillover into neighbouring keys.
**Implementation sketch.** Hand-craft instance.json; open; assert.
**ROI.** medium.

### G-29. plugin.json `name` differs from folder name

**User scenario.** Author renamed the folder but forgot to edit `plugin.json.name`.
**Concrete user actions.** Open project after rename.
**Hypothesis.** Clear error.
**Likely actual behavior.** Mixed: factory loads, `xi::use("folder_name")` doesn't resolve; F-P1-6: SDK mute on `unknown_command` shape; B-P1-1 leaves dead adapter.
**Bug prediction.** B-P1-1, F-P1-6.
**Success criteria for the test.** Open project surfaces `name_mismatch` warning; `xi::use` returns a clearly-failing proxy; no crash.
**Implementation sketch.** Mismatch + open + drive a script that uses both names.
**ROI.** medium.

### G-30. `abi_version` declared higher than host's `XI_ABI_VERSION`

**User scenario.** Plugin built against a future SDK.
**Concrete user actions.** Edit `plugin.json` to bump `abi_version` to 9999. Open project.
**Hypothesis.** Backend refuses; clear diag.
**Likely actual behavior.** P0-D3: the on-demand load path skips ABI compat check; if hit via that path, stale-future DLL loads with mismatched ABI → memory corruption.
**Bug prediction.** P0-D3, P0-D4.
**Success criteria for the test.** Plugin refused; ABI-mismatch warning; backend stays alive even after subsequent inspect calls referencing the rejected plugin.
**Implementation sketch.** Bump abi_version; open; drive script `xi::use("rejected")`.
**ROI.** high: cheap test for a P0.

---

## Resource leak / starvation

### G-31. SHM exhaustion under modest fps + plugin with image output

**User scenario.** 60 fps × 6 MB/frame source plugin in process-isolation.
**Concrete user actions.** `cmd:start fps=60` for 60 s with a process-isolated source emitting 6 MB images.
**Hypothesis.** SHM region recycles; no degradation.
**Likely actual behavior.** P0-E1: bump-only allocator exhausts in ~1.4 s; `image_data` returns nullptr; no alarm.
**Bug prediction.** P0-E1.
**Success criteria for the test.** Either explicit `out_of_shm` event or back-pressure visible in `dispatch_stats.dropped_*`. Silent zero-pixel images = FAIL.
**Implementation sketch.** Use `examples/burst_pipeline` style; assert no all-zero `vars` images received.
**ROI.** high: confirms biggest production-blocker.

### G-32. `xi::async` thread churn over 10 minutes

**User scenario.** Script with 4 `xi::async` calls per frame, 60 fps, 10 min run.
**Concrete user actions.** `cmd:start fps=60`; sample backend handle count + RSS every 30 s for 600 s.
**Hypothesis.** Stable.
**Likely actual behavior.** E-P1-3: 240 thread cycles/sec; ucrt arena fragmentation; handle count climbs.
**Bug prediction.** E-P1-3, E-P1-4.
**Success criteria for the test.** Backend RSS growth < 200 MB across 10 min; handle count slope < 1 / sec.
**Implementation sketch.** Python harness loops `c.call("get_metrics")` (or PowerShell `Get-Process`).
**ROI.** medium: needs 10-min run.

### G-33. WS reconnect cross-session subscription leakage

**User scenario.** Author's VS Code window reconnects after laptop sleep. Old subscriptions stick around.
**Concrete user actions.** (1) Connect; subscribe to `vars` for instance A. (2) Drop the WS forcibly. (3) Reconnect with a fresh client. (4) Drive runs.
**Hypothesis.** Fresh client has no carry-over subscriptions/history/errors.
**Likely actual behavior.** E-P1-2: `g_sub_*`/`g_history`/`g_recent_errors` survive disconnect → new driver gets prior session's stuff.
**Bug prediction.** E-P1-2, E-P1-1.
**Success criteria for the test.** New client receives no `vars` events until it explicitly subscribes; `cmd:get_history` returns empty.
**Implementation sketch.** Two SDK clients sequenced; second one asserts no inbound events for 3 s post-connect.
**ROI.** high: cheap reliable repro of E-P1-2.

### G-34. `block` overflow mode with no producer notify

**User scenario.** Source uses `overflow: "block"`; downstream pipeline stalls briefly.
**Concrete user actions.** Configure `block`; force a 2 s stall in `process`; resume. Drive 200 frames.
**Hypothesis.** Producer wakes when queue drains.
**Likely actual behavior.** A-P1-1: workers `pop_front` without `notify_one()` → producer stuck on cv.
**Bug prediction.** A-P1-1.
**Success criteria for the test.** All 200 frames eventually processed within 30 s; no producer-stall over 5 s after queue drains.
**Implementation sketch.** Use `examples/burst_dispatch` baseline; switch overflow mode; instrument source plugin to log emit times.
**ROI.** medium.

### G-35. Slow-loris WS handshake

**User scenario.** A flaky proxy delivers handshake bytes 1/min.
**Concrete user actions.** TCP-connect to backend WS port; send handshake bytes one per 30 s.
**Hypothesis.** Backend imposes per-call timeout, drops slow client.
**Likely actual behavior.** P0-D5: blocking `::recv` with no timeout → backend hostage; legitimate client cannot connect.
**Bug prediction.** P0-D5.
**Success criteria for the test.** A second well-behaved client connects within 5 s while slow-loris is in progress.
**Implementation sketch.** Raw socket loris from Python; second client uses SDK normally.
**ROI.** high: real DoS vector, single-file repro.

### G-36. TriggerRecorder disk usage at 1 hr × 60 fps

**User scenario.** Author leaves `trigger_recorder` enabled overnight.
**Concrete user actions.** Continuous run 60 fps for 60 min with recorder on.
**Hypothesis.** Disk usage rolls; bounded by quota.
**Likely actual behavior.** E-P1-5: 173 GB / 8h projected; manifest rebuild multi-second wedge under lock.
**Bug prediction.** E-P1-5.
**Success criteria for the test.** Disk growth < 5 GB after 60 min; manifest rebuild < 500 ms at 1 M events.
**Implementation sketch.** Run + sample disk; on stop trigger manifest export, time it.
**ROI.** medium: longer test but deterministic.

---

## Top 5 highest-ROI cases to implement first

1. **G-1 (double-Ctrl-S race)** — single test triggers four P0 findings in the
   most common dev workflow.
2. **G-31 (SHM exhaustion)** — confirms the production-blocking silent-corruption
   P0-E1 in under 60 s.
3. **G-21 (run vs unload UAF)** — direct repro of the marquee P0-AB-1/2 pair.
4. **G-22 (seq_ near-wrap synthetic)** — turns an 828-day latent bug into a
   minutes-long deterministic test via env-var seam.
5. **G-25 (SDK timeout state desync)** — F-P1-4 affects every driver author and
   only needs one slow recompile + tight timeout to reproduce.

## Cases that probably can't be automated reliably

- **G-18 (NFS-style 100ms latency)** — hard to inject deterministically; requires
  a filter driver / WinFsp delay layer that's not in CI.
- **G-23 (NTP forward skew)** — admin Set-Date is destabilising on shared
  builders; better as a unit test against the clock seam, not a system test.
- **G-32 (`xi::async` 10-min churn)** — accumulates slowly; flaky thresholds on
  shared CI; better as a soak suite that runs nightly.
- **G-36 (recorder 60-min)** — same wall-clock dependency as G-32.
- **G-16 (non-ASCII path)** — automatable, but VS Code extension + cl.exe
  invocation paths differ across CI images; probably needs to live in a Windows-
  specific lane.
- **G-9 (host-mutex deadlock + recompile)** — repro is cheap, but distinguishing
  "expected timeout" from "UAF survived because we got lucky" needs PageHeap /
  Application Verifier enabled, which not every developer can run locally.
