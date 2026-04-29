# stereo_sync2 PLAN

## Goal
Validate trigger-bus correlation across TWO independent source-plugin
instances of the SAME plugin type (`synced_cam`), instances `cam_left`
and `cam_right`. The framework should pair their emits by `tid` and
deliver one combined event to the script per cycle.

## Design

### Tid coordination across two instances
Two separate worker threads (one per instance) cannot share a tid via
some random generator — they need to agree. Strategy: **deterministic
TID derived from the sequence number and a shared epoch.**

Both instances:
- Read a wall-clock-anchored epoch (system steady_clock at instance ctor
  is too local; use system_clock).
- Each tick computes `seq = floor((now - epoch) / period_ms)` and
  uses `tid = {hi: 0xC0FFEE, lo: seq}`.

Refinement: rather than depending on simultaneous startup, we use a
fixed wall-clock epoch derived from a project-wide constant (or just
"epoch 0" — `seq = system_now_ms / 50`). Both cameras then naturally
emit identical TIDs at each 50 ms boundary, regardless of when each
worker started.

A worker that's slightly off-phase will sleep until the next
50 ms boundary at the top of each loop. This keeps both emitters in
phase with the global clock and produces matched TIDs.

The seq number we **embed in the frame** is the same `seq` we use for
the TID's lo field, so the script can independently confirm the
correlation worked.

### Trigger policy
Project policy: `all_required` with `required_sources = ["cam_left",
"cam_right"]` and a window large enough (e.g. 200 ms) to let a slightly
delayed emitter still match its peer.

### Isolation
**Source plugins MUST set `"isolation": "in_process"`** per
docs/reference/host_api.md and docs/guides/adding-a-plugin.md — the
default `process` isolation routes emit_trigger to a stub that no-ops.

### Script (`inspect.cpp`)
- `auto t = xi::current_trigger();`
- `t.image("cam_left")`, `t.image("cam_right")`
- Read first 4 bytes as little-endian uint32 = sequence number.
- `VAR(left_seq, ...)`, `VAR(right_seq, ...)`,
  `VAR(matched, left_seq == right_seq)`.

### Driver (`driver.py`)
1. Connect to ws://localhost:7823.
2. `open_project`.
3. `compile_and_load("inspect.cpp")`.
4. Issue `cmd:start` with `fps` so the worker blocks on the trigger bus.
5. Start a thread that pumps `cam_left` / `cam_right` exchanges
   ("start") to begin emitting.
6. Drain `_inbox_vars` for ~5 s, recording each cycle's match.
7. `cmd:stop`. Validate ≥30 cycles, ≥95% matched, cycle count
   ~ 100 ± 20.

## Open risks
- Whether the script gets dispatched correctly under continuous mode
  with `all_required` policy (`xi::current_trigger()` should be
  populated since the worker sets `g_current_trigger` to the trigger
  event before calling `run_one_inspection`).
- Whether vars from continuous-mode runs get pushed to `_inbox_vars`
  the same way as on-demand runs (looking at run_one_inspection
  it uses run_id 0 implicit; the `vars` text-message is unconditional).
- Whether the auto-respawn / isolation pieces interfere when the
  source plugin is `in_process` while other plugins (none in this
  project) are not.
