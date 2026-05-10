# Audit Fix Progress (started 2026-05-10)

19 PRs landed in this session. Tracks which audit findings (round 1 +
round 2) have been fixed, are pending user decision, or are
intentionally deferred.

## Phases

| # | Theme | Status | PR |
|---|-------|--------|----|
| 1 | SDK contract + doc drift | ✅ done | #35 |
| 2 | Wire-format bounds + UB | ✅ done | #36 |
| 3 | Run lifecycle events     | ✅ done | #37 |
| 4 | Hot-mutator drain (P0 cluster) | ✅ done | #38 |
| 5 | SHM exhaustion visibility (allocator pending Q1) | ✅ partial | #39 |
| 6 | Watchdog steady_clock    | ✅ done | #40 |
| 7 | Batch P1s (block-notify, drain, WS reconnect, gmtime_s) | ✅ done | #41 |
| 8 | ProcessInstanceAdapter respawn correctness | ✅ done | #42 |
| 9 | JSON write escape + atomic_write check | ✅ done | #43 |
| 10 | IPC seq wrap | ✅ done | #44 |
| 11 | Bound blocking recvs (accept_one, WS handshake) | ✅ done | #46 |
| 12 | open_project ABI / recompile old-ABI restore | ✅ done | #47 |
| 13 | start_reader inflight_mu_ hardening | ✅ done | #48 |
| 14 | open_project + recompile cleanup symmetry | ✅ done | #49 |
| 15 | SHM version handshake + process_mu_ | ✅ done | #50 |
| 16 | extract_string / detail_find_key → cJSON | ✅ done | #51 |
| 17 | Per-frame-type payload caps | ✅ done | #52 |
| 18 | OVERLAPPED event handle reuse | ✅ done | #53 |

## Open questions (need user decision)

### Q1. SHM allocator design (P0-E1)

`xi::ShmRegion` is bump-only. PR #39 ships visibility (loud OOM
log); proper free-list is pending design alignment.

Options: (1) per-size-class buckets (recommended), (2) general
free-list with first-fit walk, (3) buddy allocator, (4) larger
region + per-source quotas.

### Q2. `script_build/` retention policy (P0-E2)

Each compile mints `<stem>_v<N>.dll/.lib/.obj/.log`; never deletes.
~5 GB/day at typical dev cadence. Options: keep latest N, delete on
success only, time-based prune.

### Q3. `TriggerRecorder` retention policy (P1-E5)

Recorder writes raw frames at full resolution. 8 hours @ 60 fps × 6 MB
= 173 GB. No quota. Options: rolling buffer, manual-stop only
(current), time-based retention.

### Q4. C-P1-6 emit_trigger refcount race (interlocked with Q1)

Worker's `image_release` runs after `send_frame` but before host's
`TriggerBus::emit` addrefs. Latent today (bump-only SHM keeps slot
intact at refcount=0); becomes exploitable when Q1's free-list lands
and slots can be reaped at refcount=0. Fix needs co-design between
worker (drop release-after-send) and host (treat received frame as
ownership-transfer, skip addref). Defer until Q1's allocator choice.

### Q5. C-P1-2 recv_frame partial-body indefinite block

Header arrives with `len = 16 MB`; peer sends 1 KB then crashes.
With no in-flight RPC driving the timeout, the reader thread blocks
indefinitely. Fix needs choice between: (a) heartbeat / keepalive
frames, (b) per-recv inactivity timeout (typical: 30 s), (c) bound
total recv_frame duration. Each has tradeoffs (extra wire traffic,
false positives on slow links, false-negatives on stuck plugins).

### Q6. D-P1-9 CancelIoEx all-pending

`CancelIoEx(pipe_, nullptr)` cancels every pending I/O on the
handle. Today only one writer at a time; burst parallelism (task
#71) explicitly intends multiple. Fix needs per-OVERLAPPED tracking
or per-direction handle pairs. Defer until task #71 lands.

### Q7. D-P1-4 cert TOCTOU

Plugin cert SHA reads the DLL on disk, then LoadLibrary loads the
DLL on disk. Replacement between → cert validates a different file
than what gets loaded. Fix needs: compute SHA from the loaded
in-memory image, not the file path. Touches `xi_cert.hpp` API.

### Q8. E-P1-3 xi::async per-call thread

`xi::async` spawns a fresh OS thread per call via
`std::async(launch::async)`. 4 calls/frame × 60 fps = 14.4 M
thread-cycles/day → ucrt arena fragments over weeks. Fix needs:
thread pool. The header explicitly marks this as TODO; design
question is pool sizing strategy.

## Audit findings — final status

### P0 (8 of 11 closed)
- [x] **P0-AB-1..5**  hot mutator vs live caller (PR #38)
- [x] **P0-C1**  Pipe::accept_one INFINITE wait (PR #46)
- [x] **P0-C2**  seq_ wraps to 0 (PR #44)
- [x] **P0-C3**  start_reader race hardening (PR #48)
- [x] **P0-D1**  Reader::str/bytes unbounded length (PR #36)
- [x] **P0-D2**  SHM alloc_image int32 overflow (PR #36)
- [x] **P0-D3**  open_project plugin auto-load skips ABI (PR #47)
- [x] **P0-D4**  recompile old-ABI restore gap (PR #47)
- [x] **P0-D5**  WS handshake slow-loris (PR #46)
- [ ~ ] **P0-E1**  SHM bump-only — visibility shipped (#39); allocator pending Q1
- [ ] **P0-E2**  script_build/ accumulation — pending Q2

### P1
- [x] A-P1-1  overflow:"block" missing notify_one (PR #41)
- [x] A-P1-2  cmd:start consumes stale events (PR #41)
- [x] B-P1-1/2  open_project partial-failure cleanup (PR #49)
- [x] B-P1-4  recompile early-returns null instances (PR #49)
- [x] B-P1-7  process_via_rpc out_* outside mu_ (PR #50)
- [x] C-P1-1  SHM no version handshake (PR #50)
- [ ] C-P1-2  recv_frame partial-body indefinite block — Q5
- [x] C-P1-4  respawn cap counts CreateProcess fails (PR #42)
- [x] C-P1-5  SET_DEF restore failure (PR #42)
- [ ] C-P1-6  emit_trigger refcount race — Q4 (latent today)
- [x] C-P1-7  per-type payload cap (PR #52)
- [x] D-P1-1  JSON write no escape (PR #43)
- [x] D-P1-2  extract_string substring match (PR #51)
- [x] D-P1-3  gmtime not thread-safe (PR #41)
- [ ] D-P1-4  cert TOCTOU — Q7
- [ ~ ] D-P1-5  atomic_write return ignored — partial in PR #43; other sites pending
- [x] D-P1-6  (size_t)(-n) UB pattern (PR #36)
- [x] D-P1-7  ImagePool::create overflow (PR #36)
- [x] D-P1-8  ImagePool counter drift (PR #36)
- [ ] D-P1-9  CancelIoEx all-pending — Q6 (latent today)
- [x] D-P1-10  system_clock for watchdog (PR #40)
- [ ] D-P1-11  WS HMAC system clock 60s — by-design (threat model)
- [x] E-P1-1  g_iso_dead_reported never cleared (PR #41)
- [x] E-P1-2  WS reconnect cross-session contamination (PR #41)
- [ ] E-P1-3  xi::async per-call thread — Q8
- [x] E-P1-4  CreateEventA per chunk (PR #53)
- [ ] E-P1-5  TriggerRecorder unbounded — pending Q3
- [x] F-P1-1  run_started/finished/error not emitted (PR #37)
- [x] F-P1-2  SDK compile_and_load docstring wrong (PR #35)
- [x] F-P1-3  SDK compile_and_load no timeout kwarg (PR #35)
- [x] F-P1-4  c.call timeout raises bare Empty (PR #35)
- [x] F-P1-5  next_vars None on disconnect (PR #35)
- [x] F-P1-6  exchange_instance no unknown_command surface (PR #35)

## Tally

- **Closed**: 9 P0 + 22 P1 = **31 findings** (or 33 counting partials)
- **Pending user decision (Q1-Q8)**: 8 items, all design-needed
- **Intentional deferral**: D-P1-11 (HMAC by-design)

19 PRs in this session: #35 through #53.
