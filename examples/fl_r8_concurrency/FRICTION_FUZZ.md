# FRICTION_FUZZ — FL r8 concurrency fuzz survey

Concurrency-surface findings ranked P0/P1/P2. Per FL convention,
nothing in this survey was fixed in-PR — fixes follow in a separate
commit / PR after the parent triages.

## P1 — RESOLVED 2026-05-09

### close_project after open(multi_source_surge) returns Empty / closes WS

**Status: FIXED.** Root cause was teardown ordering in
`xi::PluginManager::close_project` (`backend/include/xi/xi_plugin_manager.hpp`):
the function called `FreeLibrary()` on each project plugin's DLL
**before** `project_ = ProjectInfo{}` destroyed the in-process
`CAbiInstanceAdapter`s. Each adapter's destructor calls the plugin's
`destroy_fn`, which lives in the just-freed DLL → access of unmapped
memory → ACCESS_VIOLATION (`0xC0000005`). The WS handler thread died
mid-destructor, never sending a rsp; the SDK's `c.call("close_project")`
saw an `Empty()` queue.

The same bug existed historically in `open_project` and was fixed
there with a `// Destroy old instances FIRST` block (line ~995). The
matching fix in `close_project` was simply not applied. This PR
mirrors the open_project ordering: clear `project_.instances`
explicitly before `FreeLibrary`.

After fix:
- single-instance close: ~0.02 s
- 5-instance close (multi_source_surge): ~0.29 s
- 30-iter open/close cycle: 0 fatals, RSS growth 0.6 MB iter 5→30

### Original write-up

**Symptom.** Iter 0 of `harness_open_close_cycle.py` against
`examples/multi_source_surge`:

1. `c.open_project(...)` succeeds (project has 5 instances incl. 3
   sources running their plugin run-loops).
2. `c.compile_and_load(...)` succeeds.
3. `c.call("start", {"fps": 60})` and `c.call("stop")` both succeed.
4. `c.call("close_project", timeout=10)` raises `Empty()` — the rsp
   queue waited 10 s for a reply and got nothing.
5. The WS goes into a closed state; the next `c.ping()` raises
   `WebSocketConnectionClosedException('socket is already closed.')`.

**Reproduction.**

```
FUZZ_ITERS=2 python examples/fl_r8_concurrency/harness_open_close_cycle.py
```

`_results_open_close_cycle.json` will show:

```json
{ "iter": 0, "kind": "close_exc", "exc": "Empty()" },
{ "iter": 0, "kind": "ping_failed",
  "exc": "WebSocketConnectionClosedException('socket is already closed.')",
  "fatal": true }
```

**Suspect.** The `cmd:close_project` handler calls
`g_plugin_mgr.close_project()`. With multi_source_surge open this
must:

- stop the dispatcher pool (8 threads, currently quiesced after `cmd:stop`)
- quiesce + join 3 source plugin worker threads (the in-process
  `BurstSource::stop_()` joins its emit thread)
- destroy 5 plugin instances

If any of those `join()` calls block past the 30 s WS rsp timeout,
the rsp never reaches the SDK's queue → `Empty()`. The WS itself
seems to also get torn down somewhere in the flow (subsequent ping
fails) — possibly the backend handler thread crashed or the WS
server `close_client()` was invoked.

**Prior occurrence (this session).** While building
`examples/process_overhead/` I hit the same `c.call("close_project")`
hang and worked around it by spawning a fresh backend per
measurement. That workaround masked this bug — r8 catches it.

**Suggested next steps for whoever fixes this.**

1. Add stderr logging around `close_project()` in
   `xi_plugin_manager.hpp` to see which `join()` hangs.
2. Suspect candidate: source plugin threads with a sleep cadence
   (`run_loop_` in burst_source) that don't observe `running_=false`
   promptly enough — the dtor blocks on `thread_.join()`.
3. Cheap mitigation: cap close_project handler at, say, 2 s; on
   timeout abandon the join (detach thread) and reply error. Better
   diagnostics than a silent hang.

## F1 (friction, harness-side, not a backend bug)

### set_param_storm: harness uses fallback param names that don't exist

`harness_set_param_storm.py` queries `list_params` to discover real
param names; if `list_params` returns empty (the
`multi_source_surge` inspect script declares no `xi::Param<>`), it
falls back to `["fps", "burst", "trigger_source"]`, none of which
match a real param. The backend correctly rejects all 82,348
attempts with ProtocolError → 100% "set_errs", `sets_per_s == 0`.

This is recorded as a finding because the headline metric
("set_param throughput low") looks alarming until you cross-
reference. Useful coverage even so:

- Backend handled 82 k rejection rsps in 8 s (~10 kHz round trips)
  with no crash, no stale state, no leak.
- `c.ping()` post-storm succeeded.

**Suggested.** Future revision of this harness should either:
(a) point at a project whose script declares params (e.g.
`examples/burst_pipeline/`), or
(b) hard-code a known param name and fail fast if the project
doesn't have one. A per-iter
`set_param_returned_unknown_name_count` metric would catch this
condition without dressing it up as a backend issue.

## Negative results (no findings — surface confirmed clean)

### emit_trigger × WS cmd race
1747 emit_trigger calls + 3480 WS cmds (ping / version / list_params
/ set_param / exchange_instance) interleaved over 12 s — 0 errors,
0 dropped, max latency for either side under 20 ms. The reader
thread + dispatcher concurrency is healthy after PR #26 / #28 fixes.

### cmds during compile_and_load
While the WS handler thread was parked inside cl.exe (3.7 s
typical), 2 cmds were sent per iter (8 iters) and all 16 received
responses — just deferred until compile returned. No deadlocks, no
lost rsps. The handler-blocking pattern works (rsps queue on the
client side).

### backend_kill mid-RPC
`taskkill /F` on the backend while a slow RPC is in flight: across
4 iters, every `xinsp-worker.exe` child detected host death and
exited within **0.10–0.11 s**. No orphans on any iter.

## Assumptions made

- BackendProc spawn pattern from `examples/fl_r7_fuzz/_common.py` is
  the right starting point — copied + extended for `count_processes`
  and `proc_rss_mb` helpers.
- `multi_source_surge` is the right "rich project" target (5
  instances, multiple isolation modes, both source-style and
  detector-style plugins) to flush concurrency bugs out.
- `examples/cross_proc_trigger` is the right target for
  isolation:process worker cleanup tests (1 process-isolated source).
- Per session memory `feedback_no_kill_unknown_node`: every
  `taskkill` in `harness_backend_kill.py` checks
  `tasklist /fi imagename` first to confirm we're killing
  `xinsp-backend.exe`, NOT a node/npm process.
- Per FL convention, "do not fix bugs you find" — P1 above is
  documented but not patched in this PR.

## Out of scope (explicitly not covered)

- Multi-client concurrency. The WS server is single-client by design
  (`docs/reference/ws-protocol.md` § Single-client enforcement); 2nd-client
  rejection was already validated by FL r7 P1 fix.
- Long-soak (hours) tests. Each harness here runs ≤30 s. Memory
  leak detection is bounded to what shows up in 30 s; subtler leaks
  need a separate soak round.
- Crash-recovery semantics during `compile_and_load` (i.e. backend
  killed exactly while a compile is mid-way). Touched obliquely by
  `backend_kill` but not pre-loaded with a compile in flight.
- Plugin-side concurrency (one plugin emitting from multiple internal
  threads simultaneously) — that's plugin-author surface, not
  framework surface.

## Linux-port notes (per project policy)

`harness_backend_kill.py` uses Win32 `taskkill /F /PID` and
`tasklist /fi imagename` (gated `if os.name == "nt"` in
`_common.py::count_processes`). Linux port replaces with
`kill -9 <pid>` and `pgrep -c <name>`. Logged in
`docs/roadmap/linux-port.md` inventory entry under "fuzz harnesses".
