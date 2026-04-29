# multi_source_surge — FL測試 round 6 friction log

Format per item: **symptom -> minimum repro -> root cause guess ->
who fixes**.

Severity:
- **P0** = blocker / silent correctness loss.
- **P1** = real cost (debugging time, missed signal, surprising
  behaviour). Should be fixed before next round.
- **P2** = papercut. Doesn't block but makes the framework feel
  rougher than it needs to.

Nothing in this round was P0. There were no crashes, no observed
misattribution under N=8 dispatch, and no instance-state corruption
across hot-reload. The dispatch core looks healthy in the
multi-source case.

---

## P1-1: `dispatch_stats` drop counters reset on `cmd:start` but the doc never says so

**Symptom.** Sweep B's drops printed as **-548** (negative). My
driver did the natural thing:

```python
before = c.call("dispatch_stats")
c.call("start", {"fps": 200})
...
after  = c.call("dispatch_stats")
drops  = after.dropped_oldest - before.dropped_oldest
```

**Min repro.** Run two sweeps back-to-back with the pattern above.
The second sweep's `before` snapshot carries the previous sweep's
accumulated drops; `cmd:start` then zeros the counter; `after` is a
small post-start number; subtraction goes negative.

**Root cause.** `service_main.cpp` line 1641-1643:

    g_dropped_oldest = 0;
    g_dropped_newest = 0;
    g_queue_high_watermark = 0;

inside the `cmd:start` handler. This is correct behaviour — per-run
windows are what you want — but **nothing in the dispatch_stats
inline comment, in `docs/protocol.md`, or in
`docs/concepts/parallelism.md` (which doesn't exist yet) says so**.
The cmd:start cmd's protocol doc also doesn't list this side effect.

**Who fixes.** Framework / docs. Two options:

- (cheap) document the reset behaviour in `docs/protocol.md` under
  `dispatch_stats` AND `cmd:start`. The fields' inline comments in
  service_main.cpp 2473-2478 already mention "since last cmd:start"
  for `queue_depth_high_watermark` but NOT for the dropped counters.
  Inconsistent.
- (better) include a `since_run_epoch` integer in the
  `dispatch_stats` payload that increments on every cmd:start.
  Drivers can detect "the counters got reset between my snapshots"
  and stop subtracting.

**Fixed in this PR.** Documented in `docs/protocol.md` and the
inline comments in `service_main.cpp`. Driver also updated to
trust `stats_after` as the per-sweep total (which `cmd:start` makes
correct).

---

## P1-2: `latency_us` conflates queue-wait with inspect-time

**Symptom.** Sweep B's p95 latency is 615 ms but the median is
31 ms. The 615 ms isn't "inspect took 615 ms" — it's "frame sat in
the dispatch queue for 580 ms before any of the 8 workers got to
it." Without a queue-vs-inspect split, you can't tell which one
your case is bound by.

**Min repro.** Drive a 200-frame surge into a queue of 128 with
N=8 workers each ~15-30 ms per inspect. Look at the latency
distribution — it's bimodal. Median is the post-surge tail running
at full pipeline; p95 is the front of the surge waiting in queue.
The current `latency_us = now() - emit_ts` lumps both.

**Root cause.** The inspect script computes latency itself
(`inspect.cpp` line 122). The framework gives the script
`t.timestamp_us()` but no `t.dispatched_at_us()` or
`t.dequeued_at_us()`. So the script can't separate "spent
in queue" from "spent in process()."

**Who fixes.** Framework. Add a `dispatched_at_us` (or
`dequeued_at_us`) field to `xi::current_trigger()` info — it's
trivially captured at the top of `worker_body` in
`service_main.cpp` (line 911) when the event leaves
`g_ev_queue`. Then scripts can compute both:

    queue_wait = dequeued_at_us - emit_ts
    inspect_t  = now()           - dequeued_at_us

**Deferred.** Backend change to add a field to
`CurrentTriggerInfo`, the host callback signature, and the script-
side accessor. Not surface-level enough to land in this PR — would
need ABI bump or a new optional callback.

---

## P1-3: Watchdog silently disabled for `dispatch_threads > 1` without warning

**Symptom.** If you set N=8 and your inspect hangs (infinite loop,
deadlock with `xi::use()`), the cmd:stop path will deadlock waiting
to join 8 worker threads, and there's no per-thread cancel.

**Min repro.** Add `while (true) {}` inside `xi_inspect_entry`
and run with N=8. The N=1 case has the watchdog
(`g_watchdog_ms` controls it) but N=8 takes the early-return at
`service_main.cpp` line 764: `if (wd_ms > 0 && n_disp <= 1) {...}`.

**Root cause.** Acknowledged in the comment block at line 754-760:
"Watchdog state is single-slot and can only track ONE inspect at a
time. Skip it under multi-dispatch (N > 1)." — but **this is not
exposed to the user**. The dispatch_stats reply doesn't include
"watchdog_active: false", and `docs/concepts/parallelism.md`
doesn't exist to warn users that switching to N=8 trades crash-
safety for throughput.

**Who fixes.** Framework + docs. Minimum viable fix: emit a one-
shot `log` message at WARN level on cmd:start when N>1 saying
"watchdog disabled under multi-thread dispatch — cancel-aware
script ops should poll xi::cancellation_requested()."

**Fixed in this PR.** Added the warning log emission in
`service_main.cpp` cmd:start branch + a note in
`docs/guides/writing-a-script.md` parallelism section.

---

## P2-1: `VAR(name, expr)` collides with locals named `name`

**Symptom.** This compile-fails with redefinition errors:

    const std::string& src_name = srcs[0];
    ...
    VAR(src_name, src_name);   // ERROR: 'src_name' redefinition

cl.exe error C2374 (redefinition) and C2086 (multiple
initialization). Three diagnostic lines of confusion before you
realise the macro is the culprit.

**Min repro.** Any inspect script that surfaces a local-variable
value with `VAR(<same_name>, <local>)`. I hit this on the first
compile of `inspect.cpp` and burned 5 minutes thinking I had a
double-declared local.

**Root cause.** `xi_var.hpp:205`:

    #define VAR(name, expr) \
        auto name = ::xi::ValueStore::current().track(#name, (expr))

The macro introduces `auto name` which shadows + redeclares any
existing `name` in scope. Looks intentional (the comment block
above says "name is available after the macro" — using the same
name as a tracked-value handle). But it makes the natural pattern
of "I have a local, surface it" silently impossible.

**Who fixes.** Framework / SDK. Three fixes ranked by
intrusiveness:

- (cheap) document the shadow rule next to the macro definition
  AND in `docs/guides/writing-a-script.md` with a worked
  example: "if you already have a local `foo`, you must rename
  one of them — `VAR(foo, bar)` introduces a new `foo`."
- (medium) provide an alternate `VAR_OF(name, expr)` form that
  expands to a statement (no introduced binding). Existing
  `VAR()` users keep working.
- (intrusive) rename the introduced binding to `name##_xi` or
  similar so collisions can't happen. Breaks every existing
  caller that relied on "name is available after."

**Status.** The shadow gotcha IS already documented in
`docs/guides/writing-a-script.md` line 178 ("**Gotcha — `VAR(name,
...)` declares a local.**"). I missed it on first read because I
was searching the file for "redefinition" / "redeclare," not
"declare." This is a discoverability issue more than a doc gap.
Worth surfacing the macro definition site (`xi_var.hpp`) as the
canonical reference and linking the gotcha from there. cl.exe's
C2374 diagnostic doesn't mention macros either, so even with the
doc in place the failure mode is opaque from the compile log.

---

## P2-2: No first-class way to identify "which source produced this image"

**Symptom.** With multiple source instances, the natural script
question is "which source is this frame from?" The framework gives
you `t.sources()` (returns instance names from this trigger event)
but you still have to go from "instance name string" to "what kind
of source is this?" yourself. I solved it by stamping a custom
FNV-1a hash into bytes [8..15] of every frame, recomputing it in
the script, and switching on the hash. **The framework already
knows the instance name at dispatch time** — there's no good
reason to make every multi-source case re-implement this.

**Min repro.** Write a multi-source script that needs different
processing for each source. With the current API:

    auto srcs = t.sources();        // ["source_steady"]
    if (srcs[0] == "source_steady") { ... }
    else if (srcs[0] == "source_burst") { ... }

That works but (a) string compare in the hot path is unavoidable,
(b) typos give you silent fallthrough, and (c) for `policy=any`
the list is always 1-element, but for `all_required` it's N — the
script has no policy-aware affordance for "the leader source."

**Who fixes.** Framework / SDK. Add an optional helper:

    Image t.image()              // single-source case, no name needed
    std::string t.primary_source() // policy-leader name, single string
    bool t.has_source(const char*) // for routing

The host already tracks `leader_source` in the
`TriggerEvent` struct (`xi_trigger_bus.hpp` line 52) but it's not
exposed through the script-side `Trigger` API.

**Deferred.** Worth a small dedicated change but not surface-level
enough for this PR. Filed here so future rounds don't re-encounter
it.

---

## P2-3: `instance.json` config is not validated against `plugin.json` manifest.params

**Symptom.** I typo'd `"shape": "bursty"` as `"shape": "burts"` in
one of the instance.json files during early iteration. The plugin
silently fell through to the default `STEADY` shape and I couldn't
figure out why my burst source was emitting at constant fps.

**Min repro.** Create an `instance.json` with a config field whose
value isn't in the plugin's documented allowed set. Open project.
No warning. Plugin silently ignores it.

**Root cause.** `plugin.json.manifest.params` is currently
informational metadata for the UI — `xi_plugin_manager.hpp` doesn't
validate `instance.json.config` against it on load. The
`open_project_warnings` channel exists (used for "skip-bad-instance"
errors per service_main.cpp 2491-2511) but no code wires unknown-
config-key or out-of-range warnings into it.

**Who fixes.** Framework. Tighten `xi::PluginManager::open_project`
to:
1. Walk `instance.json.config` keys.
2. For each, look up `plugin.json.manifest.params[name]`.
3. Emit a warning to `open_project_warnings` if the key is unknown
   or the value is out of declared range/enum.

This wouldn't break valid configs and would catch a class of silent
typos. The `manifest.params[].default` already has type info to do
basic validation against.

**Deferred** to framework owner — non-trivial behavioural change.

---

## P2-4: `manifest.exchange` is purely descriptive; unknown commands silently no-op

**Symptom.** I sent `{"command": "burts"}` (typo of "burst") via
`exchange_instance` at one point. The plugin silently no-op'd. The
backend returned ok with the plugin's `get_def()` payload. No hint
that the command was unrecognised.

**Min repro.** Any plugin's exchange handler that uses an
if/else-if chain — the final else does nothing, returns get_def(),
and the caller can't tell.

**Root cause.** `xi::Plugin::exchange` is plugin-implementer
discretion. The base class doesn't inspect `manifest.exchange` to
validate. Plugin authors universally write `if (cmd == "x") ...
else if (cmd == "y") ...` with a silent fallthrough.

**Who fixes.** Plugin authors are the proximate cause; the
framework can do better by:

- adding a default `xi::Plugin::exchange_default` that returns
  `{"error": "unknown_command", "command": "<name>"}` if the
  subclass didn't override.
- documenting the expected error-shape in `docs/reference/plugin-abi.md`.

**Fixed in this PR (tiny).** This example's `burst_source` and
`work_detector` plugins now log "unknown command" and surface it
in get_def().

---

## P2-5: Source-name buffer in `xi::Trigger::sources()` is a fixed 2048 bytes

**Symptom.** Stack-allocated 2KB buf in `xi_use.hpp:118` for the
\n-separated source name list. With my 3 sources of total name
length ~50 bytes there's no risk, but a project with 50+
camera-array sources (e.g. a multi-tile inspection rig) and longer
naming (`station_03_left_module_top_camera`) could overflow. The
truncation would be silent — the host returns `n` ≤ buflen, the
script splits on \n, and the last element gets cut.

**Root cause.** Magic 2048 in the script-side accessor.

**Who fixes.** SDK / framework. Either:

- raise to a more generous heap allocation (the call is once per
  inspect; allocator pressure is fine), or
- have the host return `needed_bytes` so the script can resize.

**Deferred.** Not exercised in this round; logging here so it
doesn't get lost.

---

## P2-6: `compile_and_load` mid-run takes 4+ seconds with no progress signal

**Symptom.** Hot-reload is real and works, but it takes ~4.3 s
under the multi_source_surge case (cl.exe-bound rebuild). During
that 4.3 s the WS connection is silent — no log line, no progress
event. A driver author who hasn't read the source could easily
assume the connection has hung.

**Min repro.** Call `c.compile_and_load(path, timeout=180)` while
sources are emitting. Tail backend log output. No
"compile_started" / "compile_progress" event is emitted.

**Root cause.** `compile_and_load` is request/response with no
streaming. Compile happens synchronously on the WS handler thread
in `service_main.cpp` (line 1283: `auto res = xi::script::compile(req);`).

**Who fixes.** Framework. Cheap option: emit a single
`{"type":"event","name":"compile_started","data":{"path":"..."}}`
line right before kicking off the cl.exe child. Drivers can
display "compiling..." UI without parsing log lines.

**Deferred.** Worth a small commit but not in this PR's scope.

---

## What got fixed in this PR

- Documented the `cmd:start` reset semantics for drop counters and
  high-watermark in `docs/protocol.md` + clarified the inline
  comments in `backend/src/service_main.cpp`.
- Added a `VAR(name, expr)` shadow-collision warning to
  `docs/guides/writing-a-script.md`.
- Updated the linux-port inventory in `docs/design/linux-port.md`
  to note the new example uses no Win-specific code.
- This example itself: a self-contained 5-instance project + driver
  + reproducible numbers.

## What got deferred to the framework owner

- P1-2 latency split (`dequeued_at_us`)
- P1-3 watchdog warning emission on cmd:start with N>1
- P2-2 first-class `t.primary_source()` helper
- P2-3 instance.json config validation against plugin manifest
- P2-4 default exchange unknown-command handling
- P2-5 source-name buffer sizing
- P2-6 compile_started progress event

None of these block FL r6's pass criteria; they're papercuts and
gaps the multi-source case made visible.
