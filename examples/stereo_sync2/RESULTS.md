# stereo_sync2 — FL測試 r3 regression

## Outcome
- Cycles observed: 186 total (100 bus-driven, 86 timer-fallback ticks)
- Matched: 100 / 100 bus-driven cycles
- Backend survived 5 s soak: yes
- VERDICT: PASS

## What I observed about trigger correlation

Each `synced_cam` instance runs a worker thread that derives its TID
from a shared wall-clock epoch:

```
seq = floor(system_clock_now_ms / period_ms)
tid = { hi = 0xC0FFEE, lo = (uint64_t)seq }
```

That makes the two instances agree on the TID without any direct
coordination — both threads sleep until the next 50 ms boundary, both
read the same wall-clock millisecond, both compute the same `seq`. The
embedded LE-uint32 sequence number in pixel byte 0..3 is the same
quantity, so the script can independently confirm that the bus paired
the right frames (left_seq == right_seq).

With `trigger_policy.policy = "all_required"` and
`required = ["cam_left", "cam_right"]`, the host's `TriggerBus`
accumulates per-source images keyed by tid and dispatches the
TriggerEvent only when both sources have posted. In our 5 s soak the
match rate was 100 % (100 / 100), and total bus dispatches landed on
the nose at 20 Hz × 5 s = 100.

I also saw 86 timer-fallback inspect cycles in the same window (the
backend's continuous-mode worker fires either on bus events or on a
timer tick when the bus is idle). On those, `xi::current_trigger()
.is_active()` is false. Those aren't trigger-correlated cycles, so I
filter them out in driver.py before scoring.

**Source-plugin isolation constraint.** The docs warned about it
explicitly. `docs/reference/host_api.md` "Isolation gotcha" and
`docs/guides/adding-a-plugin.md` "How do I emit images" both spell it
out: a worker-process source plugin gets a stub `emit_trigger` that
no-ops, because the bus is a singleton in the backend's address space.
I read both files before writing instance.json, so I went straight to
`"isolation": "in_process"` for cam_left / cam_right and never had to
debug an empty-bus symptom. **Docs path was sufficient.**

## Friction log

### F-1: stale `using xi::Plugin::Plugin;` declaration (self-inflicted)
- Severity: P2
- Root cause: my own edit
- What I tried: Initially I had a custom ctor that auto-started the
  worker thread; I then refactored to defer the start to `set_def()`,
  replacing the custom ctor with `using xi::Plugin::Plugin;`. I left
  the original `using` declaration in place above it, producing two
  identical inheriting-constructor declarations.
- What worked: The cl.exe diagnostic was clean ("error C2874: 'using
  declaration causes a multiple declaration of xi::Plugin::{ctor}'") —
  pointed straight at the line. Two-second fix.
- Time lost: ~1 minute.

### F-2: spurious `open_project_warnings` cmd
- Severity: P2
- Root cause: docs gap (or me speed-reading)
- What I tried: After `open_project`, I called
  `c.call("open_project_warnings")` to surface skipped instances —
  `instance-model.md` mentions "the user can read it via
  `cmd:open_project_warnings`". The backend rejected the cmd as
  unknown.
- What worked: The skipped-instance log lines (`[xinsp2] skip instance
  'cam_left': plugin 'synced_cam' not loaded`) appeared on backend
  stderr and the project-open reply itself. I removed the call and
  surfaced via the backend log.
- Time lost: ~1 minute.
- Possibly worth filing as a doc inconsistency: either the cmd should
  exist or the doc reference should drop. Logging from the backend
  here.

### F-3: continuous-mode worker also fires timer-fallback ticks
- Severity: P1 (design issue, not a bug — but tripped my naive
  acceptance check)
- Root cause: API design — `cmd:start` runs both bus-driven dispatch
  AND a timer fallback so legacy non-source pipelines still tick. My
  first acceptance-gate check counted total cycles (including timer
  ticks) and reported 191 cycles / 100 matched = 52 %.
- What I tried: filtered cycles by `active=True` (i.e.
  `xi::current_trigger().is_active()`); on the 100 bus-driven cycles
  the match rate was 100 %.
- What worked: filtering on `active=True` in the driver's analyser.
- Time lost: ~3 minutes.
- Recommendation for docs: the `cmd:start` description in
  service_main.cpp is clear once you read it ("Bus-driven worker:
  events arrive via TriggerBus sink → enqueued → worker pops and runs
  inspect with that trigger as current. Timer fallback fires too…"),
  but neither `instance-model.md` nor the script-writing guide flags
  this. Worth a sentence in the trigger-bus section pointing out that
  `cmd:start` will also tick on a wall-clock timer when the bus is
  idle, so scripts that ONLY want trigger-driven dispatches must guard
  on `xi::current_trigger().is_active()`.

## What was smooth

- Reading the docs first paid off. The host_api.md isolation gotcha
  paragraph was load-bearing — it told me the answer to the most
  expensive bug-hunt I would have otherwise had ("why is the bus
  empty?"). Same for adding-a-plugin.md "How do I emit images".
- `xi::Plugin` base class + `XI_PLUGIN_IMPL` macro: the boilerplate is
  nicely contained. `using xi::Plugin::Plugin;` is the one trick to
  remember.
- Wall-clock-derived TIDs are the right primitive for two instances of
  the same plugin to agree on a tid without any IPC. Wrote it once,
  worked first try (after the duplicate-using fix); 100 / 100 match
  rate.
- The Python client's `_inbox_vars` queue surfaces every continuous-
  mode `vars` message; `start` / `stop` cmds work as advertised. No
  threading work needed in the driver beyond a simple `while
  monotonic() < deadline` drain loop.
- 100 bus cycles in 5.0 s (exactly 20 Hz). Wall-clock-driven scheduling
  + `sleep_until` next boundary kept jitter low enough that no cycle
  was lost across the soak.
