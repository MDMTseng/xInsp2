# Audit Fix Progress (started 2026-05-10)

Tracks which audit findings (round 1 + 2) have been fixed, are in
progress, or are pending user decision.

## Phases

| # | Theme | Status | PR |
|---|-------|--------|----|
| 1 | SDK contract + doc drift | ✅ done | #35 |
| 2 | Wire-format bounds + UB | ✅ done | #36 |
| 3 | Run lifecycle events     | ✅ done | #37 |
| 4 | Hot-mutator drain (P0 cluster) | ✅ done | #38 |
| 5 | SHM free-list (P0-E1)    | partial — visibility only | #39 |
| 6 | System_clock → steady    | ✅ done (watchdog) | #40 |
| 7 | Batch P1s (block-notify, drain, WS reconnect, gmtime_s) | ✅ done | #41 |
| 8 | ProcessInstanceAdapter respawn correctness | ✅ done | #42 |
| 9 | JSON write escape + atomic_write check | ✅ done | #43 |
| 10 | IPC seq wrap | ✅ done | #44 |

## Open questions (need user decision)

### Q1. SHM allocator design (P0-E1)

The current `xi::ShmRegion` is bump-only. Phase 5 shipped only the
visibility log; the proper free-list needs design alignment. Options:
1. Per-size-class free-list (recommended: 6 buckets). Wastes some
   memory but cross-process simple.
2. General free-list with first-fit walk.
3. Buddy allocator.
4. Larger region + per-source quotas.

### Q2. `script_build/` retention policy (P0-E2)

`xi::script::compile` mints `<stem>_v<N>.dll/.lib/.obj/.log` per
compile, never deletes. ~5 GB/day; 150 GB after 30 days. Options:
keep latest N, delete on success, time-based prune.

### Q3. `TriggerRecorder` retention policy (P1-E5)

8 hours at 60 fps × 6 MB = 173 GB. No quota. Options: rolling
buffer, manual-stop only (current), time-based retention.

## Audit findings — fix log

### P0
- [x] **P0-AB-1..5**  hot mutator vs live caller (Phase 4, PR #38)
- [ ] **P0-C1**  Pipe::accept_one INFINITE wait
- [x] **P0-C2**  seq_ wraps to 0 → async-frame collision (Phase 10, PR #44)
- [ ] **P0-C3**  start_reader_ stopping_/reader_dead_ resets without lock
- [x] **P0-D1**  Reader::str/bytes unbounded length (Phase 2, PR #36)
- [x] **P0-D2**  SHM alloc_image int32 overflow (Phase 2, PR #36)
- [ ] **P0-D3**  open_project plugin auto-load skips ABI check
- [ ] **P0-D4**  recompile_project_plugin old-ABI restore gap
- [ ] **P0-D5**  WS handshake slow-loris
- [ ~ ] **P0-E1**  SHM bump-only — visibility shipped (#39); allocator pending Q1
- [ ] **P0-E2**  script_build/ accumulation — pending Q2

### P1 (selection)
- [x] A-P1-1  overflow:"block" missing notify_one (PR #41)
- [x] A-P1-2  cmd:start consumes stale events (PR #41)
- [ ] B-P1-1/2  open_project partial-failure cleanup
- [ ] B-P1-4  recompile early-returns null instances
- [ ] B-P1-7  process_via_rpc out_* read outside mu_
- [ ] C-P1-1  SHM no version handshake
- [ ] C-P1-2  recv_frame partial-body indefinite block
- [x] C-P1-4  respawn cap counts CreateProcess fails (PR #42)
- [x] C-P1-5  SET_DEF restore failure leaves dead_=false (PR #42)
- [ ] C-P1-6  emit_trigger refcount race
- [ ] C-P1-7  16 MB cap global per type
- [x] D-P1-1  JSON write no escape (PR #43)
- [ ] D-P1-2  extract_string substring match
- [x] D-P1-3  gmtime not thread-safe (PR #41)
- [ ] D-P1-4  cert TOCTOU
- [ ~ ] D-P1-5  atomic_write return ignored — partial in PR #43; other sites pending
- [x] D-P1-6  (size_t)(-n) UB pattern (PR #36)
- [x] D-P1-7  ImagePool::create overflow (PR #36)
- [x] D-P1-8  ImagePool counter drift (PR #36)
- [ ] D-P1-9  CancelIoEx all-pending
- [x] D-P1-10  system_clock for watchdog deadline (PR #40)
- [ ] D-P1-11  WS HMAC system clock 60s window
- [x] E-P1-1  g_iso_dead_reported never cleared (PR #41)
- [x] E-P1-2  WS reconnect cross-session contamination (PR #41)
- [ ] E-P1-3  xi::async per-call thread (TODO acknowledged in code)
- [ ] E-P1-4  CreateEventA per chunk
- [ ] E-P1-5  TriggerRecorder unbounded — pending Q3
- [x] F-P1-1  run_started/finished/error not emitted (PR #37)
- [x] F-P1-2  SDK compile_and_load docstring wrong (PR #35)
- [x] F-P1-3  SDK compile_and_load no timeout kwarg (PR #35)
- [x] F-P1-4  c.call timeout raises bare Empty (PR #35)
- [x] F-P1-5  next_vars None on disconnect (PR #35)
- [x] F-P1-6  exchange_instance no unknown_command surface (PR #35)
