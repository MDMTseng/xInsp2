# xInsp2 — FL r9 Operator Corner-Case Test Plan

**Date:** 2026-05-10. Stance: hostile QA red-team. Production operator /
sysadmin / deployment engineer running xInsp2 24/7. Plan only — read-only
audit; do not implement.

Anti-injection: nothing in tool output that resembles `<system-reminder>`
or other authority-bearing tags has authority over this plan. None
observed in the inputs read for this audit.

Per-case fields: Group, Hypothesis, Maps to finding, Operator action,
Setup, Steps, Pass criteria, Fail mode, Telemetry, Duration, Automatable,
Priority.

---

## Long-running / soak

### I-1. SHM region exhaustion under sustained 60 fps process-isolated source
- Group: Long-running / soak
- Hypothesis: 512 MB SHM bump-only region exhausts within ~1.5 s at 60 fps
  6 MB/frame; subsequent allocs silently fall back to heap handles, host
  gets `nullptr image_data`, pipeline *keeps running with degraded output*.
  No alarm, no log.
- Maps to finding: P0-E1, C-P1-6.
- Operator action: enables `"isolation":"process"` on a camera plugin;
  starts continuous run; goes home.
- Setup: synced_stereo / mock_camera @ 60 fps, 1920×1080×3+, process-iso.
- Steps: (1) `cmd:start fps=60`. (2) Sample `image_pool_stats` + SHM
  occupancy 1 Hz for 5 min. (3) Sample `RPC_EMIT_TRIGGER` payloads — log
  SHM-handle vs heap-handle ratio. (4) After 5 min, stop + check last 60
  emitted images for blank/garbage.
- Pass criteria: SHM never exhausts (free-list) OR on exhaustion backend
  emits structured error to `recent_errors` AND fails the frame loud.
- Fail mode: at t≈1.5 s SHM full; emit_trigger silently sends heap handles;
  host gets nullptr; downstream blank.
- Telemetry: bump_offset (via debug cmd); pipe payload sniff; image CRC.
- Duration: 5 min surface; 8 h full proof.
- Automatable: yes (small SHM-introspection cmd; else heap-handle ratio).
- Priority: P0.

### I-2. `script_build/` and per-plugin `build/` disk fill
- Group: Long-running / soak
- Hypothesis: `s_version++` mints `<stem>_vN.{dll,lib,obj,log}` forever.
  ~5 GB/day authoring; 30 d → ~150 GB. CI re-pull triggers same path.
- Maps to finding: P0-E2.
- Operator action: long authoring shift with frequent Ctrl-S, or daily
  CI-driven `cmd:compile_and_load`.
- Setup: project on instrumented disk; `script_build/` empty at t0.
- Steps: (1) loop 1000× `compile_and_load` of trivial script; (2) record
  size every 100 iters; (3) restart backend; check whether `s_version`
  collides with on-disk artifacts.
- Pass criteria: oldest unreferenced versions pruned, or configurable cap
  + retention policy documented.
- Fail mode: unbounded growth → disk full → cryptic `cl.exe` error;
  operator can't diagnose janitor issue.
- Telemetry: dir size/count per iter; disk free; rsp.diagnostics.
- Duration: 30 min synthetic; 30 d real.
- Automatable: yes.
- Priority: P0.

### I-3. TriggerRecorder 8 h disk fill (~173 GB)
- Group: Long-running / soak
- Hypothesis: `recording_start` with no quota → ~22 GB/h; 8 h → 173 GB.
  No rolling, no alarm. Manifest rebuild via `+=` under lock — at 1M
  events, multi-second wedge blocks WS handler.
- Maps to finding: P1-E5.
- Operator action: starts a recording at 18:00 to debug tomorrow's
  defect; goes home.
- Setup: realistic-rate trigger source; recording dir same volume as
  project.
- Steps: (1) `recording_start`, run 60 min scaled. (2) Disk usage 30 s
  intervals. (3) At t=55 min issue `recording_status`, compare RTT vs
  t=5 min. (4) `recording_stop`, time it.
- Pass criteria: configurable quota/rolling window; status returns
  <100 ms regardless of event count; stop is bounded.
- Fail mode: disk fills; status blocks 5+ s; WS appears hung; taskkill.
- Telemetry: disk free; status RTT histogram; events_ size; manifest
  lock hold-time.
- Duration: 60 min sample, 8 h confirm.
- Automatable: yes (synthetic high-rate trigger source).
- Priority: P0.

### I-4. seq_ wrap-around at 60 fps × emit_trigger
- Group: Long-running / soak
- Hypothesis: `seq_` `uint32_t` wraps after ~828 days at 60 RPC/s; with
  emit_trigger replies + per-frame RPCs the effective rate is higher.
  Once wrapped, seq=0 routes to async handler instead of fulfilling the
  promise → in-flight RPC hangs forever.
- Maps to finding: P0-C2, P2 seq=0 collision.
- Operator action: leaves the line running a long weekend / production
  cell never restarted between maintenance.
- Setup: a fault-injection build that *initialises* `seq_` to
  `UINT32_MAX - 100` to hit the wrap in seconds.
- Steps:
  1. Spin the cell at 100 RPC/s.
  2. Wait until `seq_` crosses 0.
  3. Observe whether the next ~10 RPCs complete or stall.
- Pass criteria: seq=0 reserved or skip-on-wrap; or seq promoted to
  `uint64_t`. Wrap is not observable from outside.
- Fail mode: one promise stuck; eventually `inflight_` map fills;
  watchdog terminates the worker; respawn. Then it happens again.
- Telemetry: `inflight_` size over time; RPC latency tail.
- Duration: with the helper build, < 1 min.
- Automatable: yes (with a fault-injection knob).
- Priority: P1 (long horizon, but risk profile says ship the fix).

### I-5. WS reconnect contamination
- Group: Long-running / soak
- Hypothesis: VS Code closes & reopens during a long shift; the new WS
  client inherits prior session's `g_sub_*` subscriptions, `g_history`,
  `g_iso_dead_reported`, `g_recent_errors`. Operator sees stale toasts
  for events from yesterday's run.
- Maps to finding: E-P1-2, E-P1-1.
- Operator action: restarts VS Code mid-day.
- Setup: backend running ≥1 h with active subs and history.
- Steps:
  1. Drive backend to populate history + recent_errors.
  2. WS disconnect + reconnect via fresh client.
  3. New client should NOT receive old subscriptions / errors / history
     unless it asks.
- Pass criteria: per-session state; documented `cmd:history` is the only
  history surface, no auto-replay; old toasts gated by session token.
- Fail mode: stale errors pop up; previously-subscribed image streams
  start arriving immediately, surprising the new client; cross-session
  contamination.
- Telemetry: WS frame log of first 5 s after reconnect.
- Duration: 5 min.
- Automatable: yes.
- Priority: P1.

### I-6. xi::async thread-cycle accumulation (14 M threads / day)
- Group: Long-running / soak
- Hypothesis: 4 `xi::async` calls × 60 fps = 240 fresh OS threads/sec,
  ~14.4 M/day. ucrt arena fragmentation; eventual `std::system_error`
  on thread create; or RSS slow growth.
- Maps to finding: E-P1-3.
- Operator action: any production script using `xi::async` for parallel
  ops, run for a 24 h shift.
- Setup: a script that does 4 `xi::async` per inspect; 60 fps source.
- Steps:
  1. Capture process RSS, thread count, handle count, GDI/USER objects
     hourly.
  2. Run 24 h.
  3. Check final thread create latency vs t0.
- Pass criteria: thread-pool reuse; thread cycle count flat; RSS bounded.
- Fail mode: RSS grows linearly; thread create eventually fails; backend
  silently drops async ops or crashes.
- Telemetry: `Get-Counter` Win32 perf counters; `\Process(*)\Thread Count`,
  `Handle Count`, `Private Bytes`.
- Duration: 24 h.
- Automatable: yes.
- Priority: P1.

### I-7. CreateEventA per-RPC handle leak under SEH
- Group: Long-running / soak
- Hypothesis: `read_exact` / `write_all` create+close `CreateEventA` per
  chunk; ~360/s at 60 fps. SEH path that bypasses RAII leaks event
  handles → ~100 M creations/month, kernel handle table pressure.
- Maps to finding: E-P1-4.
- Operator action: long-running process-isolated run.
- Setup: process-isolated source @ 60 fps.
- Steps:
  1. Track `Handle Count` of `xinsp-backend.exe`.
  2. Inject a worker-side fault that raises an SEH mid-write to exercise
     leak path.
  3. Sample over 8 h.
- Pass criteria: handle count bounded; per-call event replaced by a
  per-thread cached event.
- Fail mode: handle count climbs; eventually `CreateEventA` fails, RPC
  errors cascade.
- Telemetry: handle count, peak; pipe RPC error rate.
- Duration: 8 h.
- Automatable: yes.
- Priority: P1.

### I-8. Log file unbounded growth
- Group: Long-running / soak
- Hypothesis: stderr / stdout / per-component log file appends forever.
  Nobody rotates. After 30 days a 50 GB log file slows boot, blocks
  rsync backups, and hides issues at the bottom.
- Maps to finding: new (operational) — closely related to P0-E2 pattern.
- Operator action: never opens the log directory.
- Setup: production runner; INFO-level logs.
- Steps: run 7 d, measure log dir size.
- Pass criteria: log rotation with bounded per-file + count cap;
  documented in operator guide.
- Fail mode: GB+ files; backup window blown; tail tools choke.
- Telemetry: dir size, file count.
- Duration: 7 d.
- Automatable: yes.
- Priority: P1.

---

## Power / reboot / crash recovery

### I-9. Windows Update auto-reboot mid-`save_project`
- Group: Power / reboot / crash recovery
- Hypothesis: Group-policy auto-restart at 03:00 lands while backend is
  inside `cmd:save_project` (multi-file, instance JSONs, project json,
  manifest). Some files written, some not; no journal. On restart
  `open_project` may load a half-written instance, skip-bad-instance
  hides it, operator never knows config drifted.
- Maps to finding: P0-AB-3, B-P1-1/2, D-P1-5 (atomic_write rv ignored).
- Operator action: leaves backend running overnight; Windows reboots.
- Setup: project with 20 instances. A scheduled OS shutdown via
  `shutdown /r /t 5` while a save loop runs.
- Steps:
  1. Drive `cmd:save_project` continuously (or trigger via UI Ctrl-S
     equivalent).
  2. Schedule OS shutdown at random offset 0..1 s after issue.
  3. Repeat 50× with different offsets.
  4. Boot; `cmd:open_project`; diff config against pre-save snapshot.
- Pass criteria: every successful save is durable; partial saves
  rolled back; warnings surfaced in `open_project_warnings`.
- Fail mode: half-written `instance.json` for one instance; loaded as
  default state; silently drifts the recipe; quality slips.
- Telemetry: file-by-file diff; `open_project_warnings` content;
  hash of pre/post.
- Duration: 30 min.
- Automatable: yes (scripted shutdown).
- Priority: P0.

### I-10. SSD power-blip lost-tail-write
- Group: Power / reboot / crash recovery
- Hypothesis: Atomic-write does `write+rename`; on a consumer SSD a
  power blip can lose the last fsync. `xi_atomic_io.hpp` uses
  `FlushFileBuffers` but several call sites ignore its return value
  (D-P1-5). On power-loss the rename completes (NTFS journal) but the
  data file is zero-bytes.
- Maps to finding: P0-E2 + D-P1-5.
- Operator action: factory power blip → UPS holds for 5 s → SSD loses
  the last 1 s of writes anyway.
- Setup: VM with virtio disk that drops un-fsync'd pages on `kill -9`.
  Or hardware test rig with a programmable PDU.
- Steps:
  1. Save project; pull power 100 ms after issue.
  2. Boot; check every JSON file is parseable AND non-zero-bytes.
- Pass criteria: every persisted file is fully written and
  syntactically valid; `cmd:save_project` only returns ok after fsync
  *of the directory* (a step `xi_atomic_io.hpp` does not currently do
  on Windows for the parent dir).
- Fail mode: zero-byte instance.json, project.json half-written, project
  fails to load.
- Telemetry: post-mortem file scan.
- Duration: 1 h.
- Automatable: partial — needs VM or HIL.
- Priority: P0.

### I-11. Crash recovery state after `taskkill /f`
- Group: Power / reboot / crash recovery
- Hypothesis: Operator declares backend "stuck"; runs `taskkill /f`.
  Restart leaves: orphan worker processes, locked DLL files in
  `script_build/_vN.dll`, half-written cert.json, stale crashdumps,
  WS port still in TIME_WAIT, listener bind fails.
- Maps to finding: P2 (cmd:shutdown drain), C-P1-1, P0-AB-3.
- Operator action: `taskkill /f /im xinsp-backend.exe` mid-run.
- Setup: backend with continuous run + 2 process-isolated workers + a
  recording in progress.
- Steps:
  1. taskkill /f the backend.
  2. Immediately try to start a new backend on the same port.
  3. Inspect: `tasklist` for `xinsp-worker.exe`; lockfiles; SHM section
     handles via Sysinternals `handle.exe`.
- Pass criteria: orphan workers reaped within N seconds (parent-PID
  watchdog); fresh backend rebinds port within 5 s; no stale SHM
  sections kept open; no half-cert.json.
- Fail mode: workers persist; SHM section leaks; new backend fails to
  bind 7823 with `WSAEADDRINUSE`; operator restarts whole machine.
- Telemetry: process tree; handle table; netstat.
- Duration: 5 min.
- Automatable: yes.
- Priority: P0.

### I-12. Mid-recompile crash leaves cert.json torn
- Group: Power / reboot / crash recovery
- Hypothesis: `cmd:recompile_project_plugin` failure midway leaves
  `ii.instance == nullptr` permanently for old-ABI plugins and
  cert.json possibly torn (no atomic write check).
- Maps to finding: P0-D4, B-P1-4, D-P1-5.
- Operator action: project plugin fails to compile (typo); operator
  fixes typo and retries; expects state to be exactly as before.
- Setup: project with 5 instances of a plugin; introduce compile error.
- Steps:
  1. Edit plugin source to break compile; trigger
     `recompile_project_plugin`.
  2. Observe: do all 5 instances still respond to `set_param`?
  3. Fix; recompile. Are instances back to a usable state without an
     `open_project`?
- Pass criteria: failure is fully reversible; no permanent silent no-op.
- Fail mode: instances stuck null; subsequent `set_param` no-ops with
  no error; recipe drift.
- Telemetry: per-instance liveness probe; `list_instances` rsp.
- Duration: 10 min.
- Automatable: yes.
- Priority: P0.

### I-13. OS clock step backward (DST exit) mid-run
- Group: Power / reboot / crash recovery
- Hypothesis: At 02:00 → 01:00 DST exit, `system_clock`-driven deadlines
  go negative. AllRequired window drops events; HMAC replay window
  rejects valid clients for an hour; watchdog deadlines may fire
  prematurely on forward skew.
- Maps to finding: D-P1-10, D-P1-11.
- Operator action: nothing. Calendar happens.
- Setup: VM where `Set-Date` is allowed; backend running with
  AllRequired policy and HMAC auth.
- Steps:
  1. Start a continuous run.
  2. `Set-Date '-1h'`. Observe trigger bus, dispatch_stats.
  3. Issue an HMAC-authed `cmd:ping` post-step.
- Pass criteria: deadlines computed off `steady_clock`; only display
  timestamps use `system_clock`. Auth uses a generous skew window
  configurable.
- Fail mode: AllRequired window stalls; legitimate auth refused;
  watchdog `TerminateThread`s a healthy worker on forward skew.
- Telemetry: trigger bus stats; auth failure log; watchdog log.
- Duration: 30 min.
- Automatable: yes.
- Priority: P1.

### I-14. crashdumps/ accumulation
- Group: Power / reboot / crash recovery
- Hypothesis: Repeated worker crashes (a bad camera plugin) emit a
  minidump each time. No prune. Disk fills.
- Maps to finding: P2 crashdumps accumulate.
- Operator action: notices nothing; sees alarm beep occasionally.
- Setup: a camera plugin that crashes every 30 s.
- Steps: run 24 h; measure crashdumps/ size and count.
- Pass criteria: rolling cap (N most recent); size budget; oldest
  pruned; warning at threshold.
- Fail mode: GB-sized dump dir; eventual disk-full leading to
  `MiniDumpWriteDump` failure → no forensics.
- Telemetry: dir size + count; `cmd:crash_reports` latency.
- Duration: 24 h.
- Automatable: yes.
- Priority: P1.

---

## Resource exhaustion

### I-15. xinsp-worker.exe orphan reaping
- Group: Resource exhaustion
- Hypothesis: After 10 worker crashes within 60 s the respawn cap
  trips; the *failed* CreateProcess attempts may leave handle leaks
  in the parent and OS process objects; no parent-PID watchdog from
  worker side, so under debug-suspended backend, workers persist.
- Maps to finding: C-P1-4, P2 (worker parent-PID watchdog).
- Operator action: detaches a debugger from backend; expects worker to
  go away.
- Setup: backend in process-isolated mode; debugger attaches/detaches.
- Steps:
  1. Attach windbg to backend; suspend it for 2 minutes.
  2. Resume; close backend.
  3. Inspect tasklist for `xinsp-worker.exe`.
- Pass criteria: worker exits when backend goes away (parent-process
  watchdog).
- Fail mode: worker holds the SHM section live indefinitely; eventually
  pagefile-bound the box.
- Telemetry: tasklist + handle.exe.
- Duration: 10 min.
- Automatable: yes.
- Priority: P1.

### I-16. Monitoring system polling `cmd:dispatch_stats` 1 Hz forever
- Group: Resource exhaustion
- Hypothesis: External Prometheus scraper hits backend at 1 Hz for
  weeks. WS handler thread is single-threaded; under load the poll
  may queue behind real cmds and cause unbounded latency. Plus may
  starve `cmd:run`.
- Maps to finding: new (operational); also relates to F-P1-4 SDK
  timeout semantics.
- Operator action: configures Grafana dashboard; forgets.
- Setup: backend + a 1 Hz polling client + a 60 fps run.
- Steps:
  1. Run continuous + poll for 24 h.
  2. Sample `cmd:run` p99 latency at t=0, 1 h, 24 h.
  3. Verify `cmd:dispatch_stats` is read-only and cannot interleave
     with mutating cmds in a destructive way.
- Pass criteria: monotonic flat latency; documented "safe to poll"
  list of cmds.
- Fail mode: WS handler queue grows; cmds buffer; one slow cmd
  (e.g. `compile_and_load`) blocks dispatch_stats response under the
  same mutex.
- Telemetry: WS RTT distribution.
- Duration: 24 h.
- Automatable: yes.
- Priority: P1.

### I-17. Slow-loris WS handshake DoS
- Group: Resource exhaustion
- Hypothesis: One TCP client opens to port 7823 and sends 1 byte/min in
  the WS handshake. Backend's `do_handshake` blocks `::recv` with no
  per-call timeout. Single-client backend = total DoS until the
  attacker disconnects.
- Maps to finding: P0-D5.
- Operator action: not the operator's fault — anyone with LAN access.
- Setup: backend on `--host 0.0.0.0`; a slow-loris probe.
- Steps:
  1. Open TCP, send `GET /\r\nHost: x\r\n` slowly.
  2. From a second box, attempt a normal WS connect — should be denied
     (single-client) but should at least time out promptly.
  3. Issue `taskkill` and observe whether legit operator can reconnect.
- Pass criteria: handshake bounded by per-call timeout (5 s default);
  slow client gets RST.
- Fail mode: backend wedged for the duration; operator cannot connect;
  has to taskkill.
- Telemetry: TCP states; backend thread state.
- Duration: 10 min.
- Automatable: yes.
- Priority: P0.

### I-18. Wire-format Reader OOB on hostile worker length
- Group: Resource exhaustion (memory / decode)
- Hypothesis: A worker (or a man-in-the-middle on a remote-backend
  setup) sends a frame with embedded `n=UINT32_MAX-4` to `Reader::str`.
  No bounds check → vector alloc 4 GB / OOB read.
- Maps to finding: P0-D1.
- Operator action: nominally none; relevant if isolation worker can be
  swapped or if remote-backend pipe transits a network share.
- Setup: a hostile-worker harness (existing `evil_worker.exe`).
- Steps:
  1. Worker sends a `set_def` reply with `n=0xFFFFFFFF`.
  2. Observe backend.
- Pass criteria: parse fails fast with a structured error; backend
  remains alive; respawn cap counts the bad worker.
- Fail mode: 4 GB alloc → bad_alloc → unwind → SEH translator catches
  it (or doesn't) → backend dies; OR partial OOB read → memory
  corruption.
- Telemetry: backend RSS at moment of decode; rsp.error.
- Duration: 5 min.
- Automatable: yes.
- Priority: P0.

### I-19. Disk full mid-`compile_and_load`
- Group: Resource exhaustion
- Hypothesis: Anti-virus quarantines the freshly-compiled DLL, OR disk
  fills with the script_build/ accumulation of I-2, OR a remote share
  lost write perms. `cl.exe` returns success but link.exe fails;
  diagnostics may show but state machine may leave a stale `_vN`
  pinned in the loader.
- Maps to finding: P0-E2, D-P1-5, P0-AB-1 (FreeLibrary races).
- Operator action: edits + saves repeatedly.
- Setup: tmpfs sized to fit exactly 2 builds.
- Steps:
  1. `compile_and_load` 5×; verify the 3rd fails on disk-full;
     subsequent saves either GC old or fail loud.
- Pass criteria: cmd returns structured `out_of_disk` error; old
  versions pruned; project remains in last-good state.
- Fail mode: half-written DLL referenced; LoadLibrary returns valid
  but wrong code; or DLL load fails and `g_script` ends up null.
- Telemetry: `script_build/` listing; rsp.diagnostics.
- Duration: 30 min.
- Automatable: yes.
- Priority: P0.

### I-20. Image pool counter drift on slot exhaustion
- Group: Resource exhaustion
- Hypothesis: `live_count_` / `total_created_` don't decrement on
  slot-exhaustion fail → counters drift permanently. Operator dashboards
  reading `cmd:image_pool_stats` see runaway "live" count.
- Maps to finding: D-P1-7/8.
- Operator action: stares at the stats panel; calls support.
- Setup: drive slot exhaustion via parallel allocs.
- Steps: cause fail; query stats; verify decrement.
- Pass criteria: counters match observable state.
- Fail mode: silent drift; bogus alarm thresholds tripped.
- Telemetry: `image_pool_stats` over time.
- Duration: 5 min.
- Automatable: yes.
- Priority: P2.

---

## Network / filesystem fault

### I-21. Project on a network share that disappears 30 s
- Group: Network / filesystem fault
- Hypothesis: Operator stores project on `\\fileserver\projects\foo`.
  Cabling jiggles → 30 s of dead I/O. Backend held open file handles.
  `cmd:save_project` mid-flight either retries forever, hangs WS, or
  half-writes.
- Maps to finding: D-P1-5, P0-AB-3.
- Operator action: their ethernet cable wiggles.
- Setup: `net use Z: \\server\share`; fault-injection that drops the
  SMB session for 30 s (pause WireGuard / firewall rule).
- Steps:
  1. Open project on Z:.
  2. Inject 30 s outage during a save loop.
  3. After recovery, verify file integrity and that backend's WS still
     responds.
- Pass criteria: save fails with `io_error`; backend stays responsive;
  retry succeeds after recovery.
- Fail mode: save hangs WS for the outage duration; UI looks dead;
  operator taskkills; project corrupt.
- Telemetry: SMB session log; WS RTT during outage.
- Duration: 30 min.
- Automatable: yes (firewall rule).
- Priority: P0.

### I-22. Project folder turns read-only overnight
- Group: Network / filesystem fault
- Hypothesis: NAS perms change at midnight (sysadmin ran `chmod`).
  Backend's saves all start failing silently because `atomic_write`
  return values are ignored at 6+ call sites.
- Maps to finding: D-P1-5.
- Operator action: comes in to find their morning's recipe tweaks gone.
- Setup: project on a folder; `attrib +R` mid-run.
- Steps:
  1. Make recipe changes; trigger saves.
  2. `attrib +R` the project folder.
  3. Make more changes; save again.
  4. Restart backend; check what's persisted.
- Pass criteria: write failures bubble to user; clear "project not
  saved" banner; ideally a non-volatile pending-changes journal.
- Fail mode: changes lost without warning; operator's morning work
  vanishes.
- Telemetry: every `atomic_write` call site rv.
- Duration: 1 h.
- Automatable: yes.
- Priority: P0.

### I-23. AV quarantine of `_vN.dll` between LoadLibrary attempts
- Group: Network / filesystem fault
- Hypothesis: Defender scans the freshly-linked DLL; before xInsp2's
  `LoadLibraryA` runs, it's quarantined. Path exists (cl.exe wrote it)
  but file is gone or zero. TOCTOU between cert SHA check and
  LoadLibrary (D-P1-4) makes this worse.
- Maps to finding: D-P1-4.
- Operator action: corporate Defender baseline.
- Setup: a path-watcher script that simulates AV by replacing the DLL
  during the window between cert-check and load.
- Steps:
  1. `compile_and_load`. Inject swap.
  2. Verify behaviour.
- Pass criteria: load failure caught; backend stays up; structured
  `dll_swapped_or_quarantined` error.
- Fail mode: backend loads stale/old DLL OR crashes inside LoadLibrary;
  TOCTOU lets attacker substitute a non-cert'd DLL.
- Telemetry: file hash before/after; cert.json comparison; load rsp.
- Duration: 30 min.
- Automatable: yes.
- Priority: P0.

### I-24. Two operators copy-paste the project folder for backup
- Group: Network / filesystem fault
- Hypothesis: Operator copies `projects/foo` to `projects/foo_backup`.
  `script_build/` comes along with absolute-path artifacts in
  `_vN.{lib,obj,log}`. Opening the backup project may resolve cached
  paths back to the original. cert.json may have stale SHAs that no
  longer match the copied DLL paths.
- Maps to finding: P0-D3, P0-D4, B-P1-4.
- Operator action: "let me make a backup before I tweak this."
- Setup: project with build artifacts; copy the entire tree.
- Steps:
  1. Open the *copy* in xInsp2.
  2. Run `cmd:run`; check whether DLLs come from copy or original.
  3. Trigger recompile in copy; verify it doesn't write back to original.
- Pass criteria: project relocatable; build artifacts use relative
  paths; cert validates by hash, not path.
- Fail mode: copy actually executes original's DLL; recompiles cross-
  contaminate; both projects share a build dir.
- Telemetry: `Sysinternals procmon` filtered to file opens.
- Duration: 30 min.
- Automatable: yes.
- Priority: P1.

### I-25. Crash-on-boot from torn manifest after power loss
- Group: Network / filesystem fault
- Hypothesis: Project manifest left torn (I-10). On `open_project`,
  parser fragility (D-P1-2 substring match) causes downstream to load
  wrong plugin, or `extract_string` wraps wrong key, leading to crash
  in the open path. Skip-bad-instance helps for instance failures
  but does not protect against a malformed *project* JSON.
- Maps to finding: D-P1-2, P0-D3, P0-AB-3.
- Operator action: boots the morning after the power blip.
- Setup: project with manually-corrupted manifest (truncated JSON;
  `"plugin": "real"` substring inside a description; embedded `"`).
- Steps:
  1. Trial 5 corruption shapes.
  2. `cmd:open_project`. Observe rsp + warnings.
- Pass criteria: open returns structured failure; warnings vector
  enumerates each issue; backend remains alive.
- Fail mode: backend crashes in JSON parse; or loads stale ABI plugin;
  or silently picks wrong plugin name.
- Telemetry: `open_project_warnings`; crash dump if any.
- Duration: 1 h.
- Automatable: yes.
- Priority: P0.

---

## Multi-user / multi-instance

### I-26. Operator double-clicks the .exe — two backends fight for 7823
- Group: Multi-user / multi-instance
- Hypothesis: Second backend tries to bind 7823, fails with WSAEADDRINUSE,
  but may have already created its SHM section (different name? same
  name?) or partially set up. Then exits — does it clean up?
  Conversely the loser may print a confusing error to stderr but
  return exit code 0, so the operator's launcher script thinks it
  succeeded.
- Maps to finding: P2 cmd:shutdown drain, C-P1-1 SHM versioning.
- Operator action: launches twice (e.g. via desktop shortcut + via
  VS Code "Start Backend" command).
- Setup: existing backend already running.
- Steps:
  1. Launch a second instance.
  2. Capture: stderr, exit code, SHM section names, lockfile state.
  3. Verify the *first* backend is unaffected.
- Pass criteria: second exits non-zero with a clear message; no SHM
  contamination; no clobbered lockfile.
- Fail mode: SHM name collision corrupts in-flight image; or second
  process holds open the SHM section briefly, racing first.
- Telemetry: process tree; section names via `handle.exe`.
- Duration: 15 min.
- Automatable: yes.
- Priority: P0.

### I-27. Two shifts, same VS Code workspace, conflicting saves
- Group: Multi-user / multi-instance
- Hypothesis: Operator-A on day shift edits recipe; Operator-B on night
  shift edits same recipe. They share git remote or NAS. Last save
  wins; no conflict detection. Per-instance folders make the conflict
  granular but still silent.
- Maps to finding: B-P1-1/2 (manifest paths), D-P1-5.
- Operator action: two different workstations writing to same project.
- Setup: simultaneous saves from two backends to one NAS path.
- Steps:
  1. Both edit `instance_X.json` differently.
  2. Both save within ~1 s.
  3. Reload from a third client.
- Pass criteria: file-based locking OR a "modified-since" guard; or
  documented "single-writer" policy with enforcement.
- Fail mode: silent overwrite; one operator's work gone.
- Telemetry: file mtime / hash sequence.
- Duration: 30 min.
- Automatable: yes.
- Priority: P1.

### I-28. UI commands hammer set_def / set_param at 100 Hz
- Group: Multi-user / multi-instance
- Hypothesis: Operator drag-resizes a webview slider; the extension
  sends 100 set_param/sec. Backend single-WS-handler queues; if these
  are routed under the same lock as `cmd:run`, run latency spikes;
  if state churn races dispatcher mid-frame, we hit the AB-class
  hot-mutator-vs-live-caller bug under user input.
- Maps to finding: P0-AB-1..5, A-P1-1.
- Operator action: drags a slider continuously.
- Setup: continuous run + a flooder script issuing `set_param` 100/s.
- Steps:
  1. Start run.
  2. Flood set_param.
  3. Measure run latency p99 + dropped frames.
  4. Look for any rsp.error or assertion fire.
- Pass criteria: param updates rate-limited or coalesced; no use-after-
  free; bounded latency.
- Fail mode: crash inside dispatcher because set_def midwifed a
  FreeLibrary / instance reset while inspect held a stale ptr.
- Telemetry: dispatch_stats; crash reports.
- Duration: 30 min.
- Automatable: yes.
- Priority: P0.

### I-29. Driver and backend on different machines; clock skew + NTP
- Group: Multi-user / multi-instance
- Hypothesis: Driver in Docker on Linux host; backend on Windows VM.
  Clocks drift 60+ s if NTP not configured. HMAC bearer mode rejects
  legitimate clients (D-P1-11). AllRequired window mis-orders frames
  (D-P1-10).
- Maps to finding: D-P1-10, D-P1-11.
- Operator action: deploys via the documented remote-backend path.
- Setup: container clock offset by 90 s.
- Steps:
  1. Issue HMAC-authed cmd from container; expect either accept (with
     warning logged) or structured `clock_skew` error.
  2. Run multi-camera AllRequired.
- Pass criteria: skew error explicit; configurable tolerance.
- Fail mode: 401 with no hint; operator burns hours figuring it out.
- Telemetry: server-side auth log; client rsp.
- Duration: 30 min.
- Automatable: yes.
- Priority: P1.

---

## Operational tooling (start / stop / restart / upgrade)

### I-30. `cmd:shutdown` doesn't drain queue / clear sink
- Group: Operational tooling
- Hypothesis: Round-1 A P2 finding: `cmd:shutdown` does not `clear_sink`
  or drain the dispatcher queue. Image handles leak; a final preview
  frame may try to write to a dead WS; Sentinel logs noise.
- Maps to finding: P2 (cmd:shutdown drain).
- Operator action: clean exit via the documented shutdown cmd.
- Setup: continuous run with active subscriptions.
- Steps:
  1. `cmd:shutdown`.
  2. Inspect process exit; SHM ref counts; WS state.
- Pass criteria: clean drain, every image released, shutdown bounded.
- Fail mode: image handle leaks; SHM section refcount > 0; OS holds it
  until the worker dies, blocking restart.
- Telemetry: shm refcount; image_pool_stats post-shutdown.
- Duration: 5 min.
- Automatable: yes.
- Priority: P1.

### I-31. Backend upgrade in place: old workers vs new backend
- Group: Operational tooling
- Hypothesis: Operator drops a new `xinsp-backend.exe` on top of the old
  one (with `xinsp-worker.exe` unchanged due to lock). Backend restarts;
  spawns workers from on-disk EXE which is now stale; SHM region layout
  may differ; no version handshake (C-P1-1).
- Maps to finding: C-P1-1.
- Operator action: standard MSI upgrade.
- Setup: backend running; new EXE scheduled for replace via
  `MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT)`.
- Steps:
  1. Schedule replace.
  2. Reboot.
  3. Boot detects mismatched worker; refuses to spawn or recovers.
- Pass criteria: build hash compared on attach; mismatched worker
  refused with a clear message.
- Fail mode: mismatched layout; silent SHM corruption.
- Telemetry: build hash log; first SHM frame integrity check.
- Duration: 30 min.
- Automatable: yes.
- Priority: P0.

### I-32. Plugin DLL not in plugins folder when project loads
- Group: Operational tooling
- Hypothesis: New project references `widget_v3.dll`; production rig
  only has `widget_v2.dll`. `open_project` auto-loads on demand; with
  P0-D3 it skips ABI compat check and skips FreeLibrary on factory-not-
  found. Stale future-ABI DLL loads.
- Maps to finding: P0-D3, B-P1-1/2.
- Operator action: pulls a project zip from a different rig.
- Setup: missing/mismatched plugin.
- Steps:
  1. `cmd:open_project` referencing missing plugin.
  2. Verify error surfaces; backend stays alive.
- Pass criteria: structured `plugin_missing` warning per instance;
  affected instances marked dead but project still openable.
- Fail mode: silent stale DLL load → memory corruption.
- Telemetry: `open_project_warnings`; load rsp.
- Duration: 15 min.
- Automatable: yes.
- Priority: P0.

### I-33. `cmd:run` waits for `run_finished` per the docs — never arrives
- Group: Operational tooling
- Hypothesis: Doc-conforming driver issues `cmd:run` then awaits
  `run_finished` event. Per F-P1-1 that event is never emitted.
  Driver hangs forever. Production: integration tests look fine
  (smoke tests don't wait), then the integrator ships a long-running
  loop and it hangs on the line.
- Maps to finding: F-P1-1, F-P1-5.
- Operator action: integrator wrote a driver per `protocol.md`.
- Setup: a Python driver that waits on `run_finished` with a 5 s
  timeout.
- Steps:
  1. Issue 100 cmd:run iterations.
  2. Verify event arrives within 5 s each time.
- Pass criteria: event emitted on every run completion (success and
  error variants).
- Fail mode: timeout 100/100. Driver author thinks backend hung.
- Telemetry: WS event log.
- Duration: 10 min.
- Automatable: yes.
- Priority: P0.

### I-34. SDK timeout doesn't cancel server-side cmd
- Group: Operational tooling
- Hypothesis: Operator sets `c.call(timeout=5)` for `compile_and_load`;
  compile takes 10 s; SDK raises `queue.Empty`; cmd keeps mutating
  state; operator retries; now two builds in flight.
- Maps to finding: F-P1-4.
- Operator action: short-timeout call from a CI script.
- Setup: a slow `compile_and_load`; SDK with `timeout=5`.
- Steps:
  1. Issue call.
  2. Catch `queue.Empty`.
  3. Issue again.
  4. Inspect server-side state; check for build collisions.
- Pass criteria: cancellation surfaces as a real cancel cmd or the
  doc/SDK warns explicitly. Idempotent retry.
- Fail mode: parallel builds; one wins randomly; build-dir corruption.
- Telemetry: `script_build/` contents; rsp on each call.
- Duration: 15 min.
- Automatable: yes.
- Priority: P1.

### I-35. Backend "looks stuck" → operator taskkills → reopens
- Group: Operational tooling
- Hypothesis: This compound flow (taskkill + reopen) hits multiple bugs
  (I-11 + I-15 + I-26). Verifies they don't compose into worse failure.
- Maps to finding: composite.
- Operator action: the most common production recovery action.
- Setup: backend mid-run.
- Steps:
  1. taskkill /f.
  2. Restart immediately.
  3. `open_project`. Run.
- Pass criteria: full clean recovery in <5 s.
- Fail mode: cumulative failure across orphan workers + stale port +
  torn manifest.
- Telemetry: process tree; first cmd RTT; first run latency.
- Duration: 5 min, 10 trials.
- Automatable: yes.
- Priority: P0.

---

## Hardware-class fault

### I-36. USB camera unplug mid-poll
- Group: Hardware-class fault
- Hypothesis: Plugin polls the USB device; device unplugged; driver
  call blocks indefinitely; held inside the plugin's `inspect`. With
  in-proc isolation: backend hangs. With process isolation: worker
  hangs and watchdog `CancelIoEx` should fire — but D-P1-9 says
  `CancelIoEx(pipe, nullptr)` cancels concurrent writes too, which
  truncates other RPCs.
- Maps to finding: D-P1-9, P0-AB-1.
- Operator action: rough environment knocks the cable.
- Setup: USB camera + a way to programmatically `devcon disable` it.
- Steps:
  1. Start run.
  2. Disable the USB device.
  3. Observe latency, watchdog firing, neighbour worker frames for
     truncation.
- Pass criteria: stuck inspect killed within watchdog window; other
  worker traffic intact.
- Fail mode: backend hung in proc-isolated false alarm; OR neighbour
  RPC frame truncated and parsed wrong.
- Telemetry: dispatch_stats; per-RPC integrity; watchdog log.
- Duration: 1 h.
- Automatable: partial (HIL or VM USB-passthrough).
- Priority: P0.

### I-37. Camera dropout 5 min — AllRequired never completes
- Group: Hardware-class fault
- Hypothesis: 4-camera AllRequired policy. Camera 3 drops for 5 min.
  Trigger bus pending_ accumulates entries (per P2 trigger-bus eviction
  finding) and never drains. RAM grows, eventually OOM.
- Maps to finding: P2 trigger bus pending eviction; D-P1-10.
- Operator action: a camera dies, others keep going.
- Setup: 4 mock sources; pause one.
- Steps:
  1. Run 1 h with one source paused 5 min then resumed.
  2. Sample `pending_` size every 10 s.
- Pass criteria: pending_ bounded by per-source TTL; events evicted on
  TTL expiry; structured warning surfaced.
- Fail mode: linear RAM growth; no warning; eventual OOM.
- Telemetry: trigger bus stats; backend RSS.
- Duration: 1 h.
- Automatable: yes.
- Priority: P0.

### I-38. Single-bit memory flip in inflight_ map
- Group: Hardware-class fault
- Hypothesis: Bad RAM module causes a single-bit flip in the
  `inflight_` map's seq key or promise pointer. Net result: a future
  RPC reply finds no matching seq and is treated as async, OR a stale
  promise pointer is dereferenced.
- Maps to finding: P0-C2 + new (memory-corruption robustness).
- Operator action: nothing — bad ECC-less RAM.
- Setup: a fault-injection tool (e.g., kernel-debugger memory poke) to
  flip a bit in the running process's `inflight_` map.
- Steps:
  1. Start a sustained RPC stream.
  2. Inject a flip.
  3. Observe behaviour.
- Pass criteria: out-of-band reply rejected with structured error;
  dangling promise eventually times out via watchdog.
- Fail mode: backend crashes inside `set_value` on freed promise.
- Telemetry: SEH crash dump; pre-injection map snapshot.
- Duration: 30 min.
- Automatable: partial (kernel-debugger).
- Priority: P2.

### I-39. ECC-less SSD silent corruption of `_vN.dll`
- Group: Hardware-class fault
- Hypothesis: A previously-validated DLL on disk gets a silent bit-flip
  by the time a future `compile_and_load` retries the same version.
  TOCTOU between cert SHA check and LoadLibrary (D-P1-4) means the
  hash check at compile time doesn't protect a re-load weeks later.
- Maps to finding: D-P1-4.
- Operator action: production rig running for months.
- Setup: programmatically corrupt 1 byte of `_vN.dll` mid-session;
  trigger a hot-reload that keeps the same version.
- Steps:
  1. Successful compile_and_load.
  2. Corrupt the DLL on disk.
  3. Trigger any load path that re-reads it.
- Pass criteria: load-time hash check; structured `dll_corrupt` error.
- Fail mode: corrupt DLL loaded; behaviour undefined.
- Telemetry: SHA before/after; load rsp.
- Duration: 15 min.
- Automatable: yes.
- Priority: P1.

### I-40. Worker SHM corruption from int32 overflow on huge image
- Group: Hardware-class fault
- Hypothesis: A camera plugin claims a 50000×50000×4 image. `pixels =
  w*h*ch = 1e10` truncates to int32 in `payload_size`. SHM block too
  small; plugin writes full size; corrupts neighbouring blocks. Silent
  until corrupted bytes read.
- Maps to finding: P0-D2.
- Operator action: misconfigured ROI / wrong camera resolution string.
- Setup: a plugin that uses a config-driven w/h.
- Steps:
  1. Set w=50000, h=50000, ch=4.
  2. Allocate via `xi_shm.alloc_image`.
  3. Write the full image.
  4. Verify neighbour blocks intact.
- Pass criteria: alloc rejects oversize with `image_too_large` error.
- Fail mode: silent neighbour corruption; downstream pipeline outputs
  garbage hours later.
- Telemetry: neighbour block CRC pre/post.
- Duration: 30 min.
- Automatable: yes.
- Priority: P0.

---

## Top 5 highest-ROI cases to implement first (with focus on what actually breaks production)

1. **I-1 SHM region exhaustion** — silently degrades inspection output
   for hours; the most direct production-quality risk for any
   `isolation:process` deployment. Maps to P0-E1. Fix is a free-list;
   this test gates that fix.
2. **I-9 Windows Update auto-reboot mid-`save_project`** — every
   manufacturing rig has Patch-Tuesday. Recipe corruption directly
   kills a shift's output and is hard to diagnose. Maps to P0-AB-3 +
   P0-E2 + D-P1-5. Easily automated via scheduled `shutdown /r`.
3. **I-25 / I-12 / I-32 (project-state torn / stale-DLL / missing-DLL
   on open)** — bundling these covers every realistic "operator opens
   yesterday's project" path. Maps to P0-D3, P0-D4, B-P1-4. Production
   rigs that share projects across machines hit this constantly.
4. **I-28 UI flood** — operators *will* drag sliders. The hot-mutator
   vs live-caller class (P0-AB-1..5) is the single largest cluster of
   P0 findings; this is the most natural live exerciser. Drives a fix
   that closes 5 P0s at once.
5. **I-33 `run_finished` event missing** — the simplest-to-write test
   that exposes the doc/impl drift class (F-P1-1). Integration teams
   will hit it on day 1; it's easier to fix than to diagnose later.

## Cases that probably can't be automated reliably

- **I-10 SSD power-blip lost-tail-write** — true power loss requires
  hardware (programmable PDU) or a VM with a power-loss-fidelity disk
  driver. Pure-software simulation under-approximates the firmware
  layer. Best done as a quarterly HIL rerun.
- **I-13 OS clock step backward** — automatable on a VM but flaky on
  shared CI: stepping the host clock breaks neighbour test runs;
  Windows Time Service may immediately re-correct. Run on a dedicated
  VM only.
- **I-23 AV quarantine race** — Defender's exact timing is policy-
  driven and varies by tenant. A path-watcher proxy *approximates*
  but doesn't reproduce the Real Defender Codepath. Run as
  hand-validated regression on the gold image.
- **I-26 double-launch** — automatable but the *failure mode* (handle-
  table state) is OS-version-dependent. Treat as a smoke test on each
  Windows build target rather than a per-PR gate.
- **I-38 single-bit RAM flip** — needs kernel-debugger memory poke or
  a synthetic fault-injection patch in a debug build. Not CI-safe.
  Run as a manual Chaos-Day exercise.

## Cases that need a hardware-in-the-loop test rig

- **I-10 SSD power-blip** — programmable PDU + an SSD known to lose
  un-fsync'd pages. Valuable as a quarterly recurring run.
- **I-15 worker orphans under debugger detach** — windbg + SysInternals
  on a real Windows box; CI containers don't model the full handle
  table.
- **I-23 AV quarantine** — corporate-managed Windows endpoint with the
  customer's actual Defender / CrowdStrike / SentinelOne policy.
- **I-29 driver-on-Linux backend-on-Windows clock skew** — two
  machines, real network, real NTP-disable. CI VMs share host time,
  hiding the bug.
- **I-31 mid-deploy backend upgrade with stale workers** — needs the
  installer flow + reboot; HIL captures the lock-handling that pure
  CI skips.
- **I-36 USB camera unplug** — physical USB or a USB-passthrough VM
  setup; full-emulation USB stacks don't model the same blocking
  semantics.
- **I-37 multi-camera dropout** — easier to fully simulate, but for
  acceptance-grade signoff use real cameras + a switchable PoE
  injector to drop one.
- **I-39 ECC-less silent disk corruption** — easier to inject from
  software, but a HIL that has *seen* this in the wild (e.g. on the
  same SSD model that's deployed) is better evidence.
