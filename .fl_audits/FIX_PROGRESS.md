# Audit Fix Progress (started 2026-05-10)

Tracks which audit findings (round 1 + 2) have been fixed, are in
progress, or are pending user decision.

## Phases

| # | Theme | Status | PR |
|---|-------|--------|----|
| 1 | SDK contract + doc drift | pending | — |
| 2 | Wire-format bounds + UB | pending | — |
| 3 | Run lifecycle events     | pending | — |
| 4 | Hot-mutator drain (P0 cluster) | pending | — |
| 5 | SHM free-list (P0-E1)    | pending | — |
| 6 | System_clock → steady    | pending | — |
| 7 | Various P1s              | pending | — |

## Open questions (need user decision)

### Q1. SHM allocator design (P0-E1)

The current `xi::ShmRegion` is bump-only — `release()` decrements
refcount but `bump_offset` never moves backward. A 512 MB region at
30 fps × 1 MB exhausts in ~17 s; after exhaustion, isolated source
plugins silently degrade (worker falls back to heap-handle which the
host's image_data can't resolve → pipeline outputs corrupt frames
without alarm).

**Phase 5 first-step shipped**: Visibility — `worker_main.cpp` now
logs to stderr the first 5 OOMs and every 200th thereafter. Silent
degradation is now LOUD degradation.

**The actual fix needs design alignment.** Options:

1. **Per-size-class free-list** (4-6 buckets, e.g. 256 KB / 1 MB /
   4 MB / 16 MB / 64 MB / 256 MB). Round alloc up to bucket. Free
   pushes onto bucket's CAS-protected stack. Simple, cross-process
   safe with ABA tag. Wastes some memory due to rounding (e.g. a
   320×240 = 75 KB image lands in the 256 KB bucket). 6× overhead in
   the worst case.

2. **General free-list with first-fit / best-fit walk.** No rounding
   waste, but CAS-pop with size search needs more care for
   cross-process correctness; harder to get right.

3. **Buddy allocator.** Compact, fast, well-known. ~100-200 lines.
   Coalescing on free reclaims contiguous space. Probably the right
   choice for production. Needs more design + test work.

4. **Larger region + per-source quotas.** Cheap workaround: make
   region 2-4 GB, accept that long runs eventually exhaust. Pair
   with a "graceful degradation" mode where the worker uses heap
   handles when SHM is full and the host knows to skip those frames.

Recommendation: option 1 (size-class free-list). Need user signoff
on the bucket sizes and on the wasted-memory tradeoff.

### Q2. `script_build/` retention policy (P0-E2)

`xi::script::compile` mints `<stem>_v<N>.dll/.lib/.obj/.log` per
compile, never deletes. ~5 GB/day at typical dev cadence; 150 GB
after 30 days under `%TEMP%/xinsp2/script_build/`. Plugin
`build/` dirs grow similarly.

Options:
- Keep latest N versions only (delete `_v<N-K>` and older).
- Delete on success; keep only on failure for diagnostics.
- Time-based prune at startup (older than M days → delete).

Each option has tradeoffs (debuggability vs disk). User to pick.

### Q3. `TriggerRecorder` retention policy (P1-E5)

Recording an 8-hour shift at 60 fps × 6 MB = 173 GB. No quota / no
rolling. Same shape of decision as Q2.

Options:
- Rolling buffer (keep last N events / last M GB).
- Manual-stop only (current behaviour).
- Time-based retention.

User to pick.

## Audit findings — fix log

(P0/P1 from rounds 1+2 listed here as resolved/pending)

### P0
- [ ] P0-AB-1..5  hot mutator vs live caller (Phase 4)
- [ ] P0-C1  Pipe::accept_one INFINITE wait
- [ ] P0-C2  seq_ wraps to 0 → async-frame collision
- [ ] P0-C3  start_reader_ stopping_/reader_dead_ resets without lock
- [ ] P0-D1  Reader::str/bytes unbounded length (Phase 2)
- [ ] P0-D2  SHM alloc_image int32 overflow (Phase 2)
- [ ] P0-D3  open_project plugin auto-load skips ABI check
- [ ] P0-D4  recompile_project_plugin old-ABI restore gap
- [ ] P0-D5  WS handshake slow-loris
- [ ] P0-E1  SHM bump-only allocator (Phase 5)
- [ ] P0-E2  script_build/ accumulation — needs retention-policy decision

### P1 (selection)
- [ ] A-P1-1  overflow:"block" missing notify_one
- [ ] A-P1-2  cmd:start consumes stale events
- [ ] B-P1-1/2  open_project partial-failure cleanup
- [ ] B-P1-4  recompile early-returns null instances
- [ ] B-P1-7  process_via_rpc out_* read outside mu_
- [ ] C-P1-1  SHM no version handshake
- [ ] C-P1-2  recv_frame partial-body indefinite block
- [ ] C-P1-4  respawn cap counts CreateProcess fails
- [ ] C-P1-5  SET_DEF restore failure leaves dead_=false
- [ ] C-P1-6  emit_trigger refcount race
- [ ] C-P1-7  16 MB cap global per type
- [ ] D-P1-1  JSON write no escape
- [ ] D-P1-2  extract_string substring match
- [ ] D-P1-3  gmtime not thread-safe
- [ ] D-P1-4  cert TOCTOU
- [ ] D-P1-5  atomic_write return ignored
- [ ] D-P1-6  (size_t)(-n) UB pattern
- [ ] D-P1-9  CancelIoEx all-pending
- [ ] D-P1-10  system_clock for deadlines
- [ ] D-P1-11  WS HMAC system clock 60s window
- [ ] E-P1-1  g_iso_dead_reported never cleared
- [ ] E-P1-2  WS reconnect cross-session contamination
- [ ] E-P1-3  xi::async per-call thread
- [ ] E-P1-4  CreateEventA per chunk
- [ ] E-P1-5  TriggerRecorder unbounded — needs retention-policy decision
- [ ] F-P1-1  run_started/finished/error not emitted (Phase 3)
- [ ] F-P1-2  SDK compile_and_load docstring wrong (Phase 1)
- [ ] F-P1-3  SDK compile_and_load no timeout kwarg (Phase 1)
- [ ] F-P1-4  c.call timeout raises bare Empty (Phase 1)
- [ ] F-P1-5  next_vars None on disconnect (Phase 1)
- [ ] F-P1-6  exchange_instance no unknown_command surface (Phase 1)
