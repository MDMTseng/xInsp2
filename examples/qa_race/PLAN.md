# qa_race — timing / concurrency / race test plan (QA agent RACE)

Viewpoint: the FE supervisor (`xinsp-fe.exe`) is a **safety** component. Its
correctness is mostly about **ordering and counting** events under a crashing /
hanging backend, not throughput. This plan maps to the FE/BE split test plan
(`docs/design/fe-be-split-test-plan.md`) §5 safety properties **SP1, SP2, SP6**
and §3 **FE-E4 / FE-E8**.

All tests are Windows-only (the FE supervisor is `#ifdef _WIN32`) and **SKIP**
(rc 0) on non-`nt`, matching `examples/fe_supervisor/driver.py`.

## How we observe the FE

The FE writes all supervisor + safe-state lines to **stderr** (the
`LoggingSafeStateSink` constructed with `nullptr` log → stderr; plus the
`std::fprintf(stderr, ...)` supervisor breadcrumbs). The drivers redirect the
FE's stdout+stderr to a `fe.log` file and assert on **line order and counts**.
Robustness rule: prefer order/count assertions over wall-clock; where a latency
bound is asserted, allow a generous margin (≥ probe_interval + several seconds).

Private ports: **7890–7909** (this agent's reserved block). Each case uses a
distinct port so cases can run back-to-back without collision.

## Fixtures (copied into this dir; BE compiles plugins from source at open)

- `projects/crashburst/` — `raw_thread_crash` plugin (copied from
  `examples/fe_supervisor`), instance `crasher` armed `true`. `process()` on
  frame 0 spawns a raw `std::thread` that null-derefs → uncatchable → BE dies on
  the first inspect. Driven at high `--autostart-fps` for the burst case.
- `projects/hang/` — `hang_forever` plugin: `process()` sleeps forever. Used by
  the (designed, manual-stub) PortUnresponsive case. See FE-E4 notes below.

## Cases

| ID | Maps to | Property under test | Pass criteria (mechanical) |
|---|---|---|---|
| **RACE-SP1** | SP1 | safe-state is driven **before** respawn | In `fe.log`, every `ENTER SAFE STATE reason=BackendExit` line appears at a **lower line index** than the *next* `respawning backend` line. No `respawning backend` line precedes its triggering `ENTER … BackendExit`. |
| **RACE-SP2** | SP2 | safe-state on **every** death incl. the cap | `count(ENTER SAFE STATE reason=BackendExit)` == number of BE deaths == `respawn_max + 1` (5 respawns attempted + the death that trips the cap = 6 BackendExit enters with default cap 5), **and exactly one** `ENTER SAFE STATE reason=RespawnLimitExceeded`. (We assert `count(BackendExit) == count(respawning) + 1` and `count(RespawnLimitExceeded) == 1`, which is robust to the exact cap value.) |
| **RACE-SP6** | SP6 | at the cap the FE **stays safe**, does not spin | After the (single) `RespawnLimitExceeded` line, there are **zero** further `respawning backend` lines in `fe.log`. FE exits on its own (rc 2) within budget; it does not loop. |
| **RACE-BURST** | SP2/SP6 + FE-E1 | rapid crash-burst respects the cap | Same project as SP-cases but `--autostart-fps=30` (crash-on-frame-0, so the BE dies almost immediately each spawn → deaths arrive as fast as the 1.5 s backoff allows). Assert `count(respawning) <= respawn_max` (==5) — the burst never exceeds the cap — and exactly one `RespawnLimitExceeded`. Proves the sliding-window limiter is not outraced by fast deaths. |
| **RACE-CTRLBREAK** | SP5/SP6 + FE-E2 | CTRL_BREAK **during a respawn-backoff window** exits cleanly, no orphan | Launch the crashburst FE in a new process group; after the FE has begun crash-looping (≥1 `respawning backend` seen in `fe.log`), send `CTRL_BREAK_EVENT` so it lands while the FE is in `Sleep(respawn_backoff_ms)`. Assert: FE exits within 10 s, `fe.log` ends with `ENTER SAFE STATE reason=SupervisorShutdown` + `supervisor stopping`, and the port is **closed** afterward (no orphaned `xinsp-backend`). Robust: we don't time the Ctrl-Break to the millisecond — we only require ≥1 respawn already logged, which (1.5 s backoff vs near-instant crash) means the handler almost always lands inside a backoff sleep; even if it lands mid-spawn the clean-exit + no-orphan assertions still hold. |
| **RACE-FE4** | FE-E4 | `PortUnresponsive` (BE alive, accept loop wedged) | **DESIGN + manual stub** (see below). Implemented as a runnable-but-`SKIP`-by-default driver that documents exactly how to drive a true accept-loop stall and what to assert. |
| **RACE-SP3-OPT** | SP3 | bounded dead-but-not-safe latency | Optional/secondary, asserted with a **generous** margin inside RACE-SP1: the first `ENTER SAFE STATE` must appear in `fe.log` within `probe_interval_ms (1000) + 10 s` of FE launch (deaths are detected by `WaitForSingleObject`, not the probe, so this is loose by design). Order/count remain the primary gates. |

## C++ unit (backend/tests/test_qa_race.cpp)

| ID | Proves | Pass criteria |
|---|---|---|
| **RACE-U1** | respawn sliding-window math (the limiter the SP/BURST cases exercise end-to-end) is correct in isolation | A pure reimplementation of `fe_main.cpp`'s window prune + cap check: 5 deaths inside a 60 s window trip the cap on the 6th; deaths spaced > window apart never trip it (mirrors FE-E8). Asserts the cap fires at exactly `respawn_max`. |
| **RACE-U2** | `SafeStateEvent` ordering invariant is representable | Building the BackendExit event then the RespawnLimitExceeded event preserves the forensic fields (module/report carried forward), matching `fe_main.cpp` lines 412–418. |

Note: RACE-U1/U2 reimplement the limiter logic locally (the real logic is a
file-static loop in `fe_main.cpp`, not yet extracted to a header). They guard the
*algorithm* the integration cases prove end-to-end; if `fe_main.cpp` extracts a
`RespawnLimiter` later, point these at it.

## FE-E4 PortUnresponsive — design (why it's a manual stub)

The `PortUnresponsive` branch (`fe_main.cpp` ~379–387) fires only when the BE
**process is alive** but the WS **accept loop stops accepting** for
`probe_fail_max` (5) consecutive probes (~5 s). A hang plugin alone does **not**
achieve this:

- `cmd:run` / continuous inspects run on **detached threads**
  (`service_main.cpp` ~1615), so a `process()` that sleeps forever wedges a
  *dispatch* thread, not the WS poll thread — the accept loop keeps accepting and
  the probe keeps succeeding. Autostart never reaches the wedge anyway.
- The poll thread is only blocked if a **synchronous command handler** blocks on
  it. `on_text → handle_command` runs on the poll thread (`service_main.cpp`
  ~3143). A command whose handler calls a plugin path that blocks forever (e.g. a
  synchronous `exchange`) would stall accept — but **autostart issues no such
  command**, and the FE has no WS client to issue one (C++ WS client is Phase 2).

**Therefore** a faithful FE-E4 needs one of (documented in the stub):
1. **A BE build flag / hang plugin that blocks the poll thread at boot** — e.g.
   a plugin whose `open`/load path (run on the poll/main thread during
   `open_project`) sleeps forever, so the accept loop never starts servicing.
   The `hang_forever` fixture here blocks in `process()`; to drive FE-E4 it must
   instead block in a constructor / `set_def` reached on the poll thread, OR
2. **A socket-level stall harness**: start a real BE, then have the test grab the
   listen backlog (open `probe_fail_max+` connections and never read) — but the
   shallow `connect` probe only checks `accept`, which the kernel completes from
   the backlog, so this does not reproduce it either.

Cleanest faithful trigger (recommended, requires a BE flag — out of this agent's
write scope): a `--hang-after-boot=MS` debug flag on the BE that, *after* binding
the port, stops calling `srv.poll()` for MS. Then: BE alive + port not accepting
→ 5 probe fails → FE logs `backend unresponsive (5 failed probes)` →
`TerminateProcess` → `ENTER SAFE STATE reason=PortUnresponsive` → respawn.

`driver_fe4.py` implements the harness shell and assertions but **SKIPs** with a
printed reason until such a BE hook exists; flip `MANUAL_HOOK_AVAILABLE = True`
once a poll-stall hook lands.
