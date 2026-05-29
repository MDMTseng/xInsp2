# FL r9 — Python SDK / Driver-Author Corner-Case Test Plan

**Stance.** Hostile QA red-team imagining the lifecycle of a real
Python driver author who is integrating xInsp2 into an existing
codebase, copy-pasting from one driver to the next, sometimes
mishandling exceptions, using async patterns, retrying on errors, and
deploying as a long-running service.

**Scope.** Tests target the boundary between a driver author's Python
code and `tools/xinsp2_py/xinsp2/client.py` + the WS protocol. Bugs
this plan hunts: SDK contract drift, error-shape mismatches, lifecycle
holes, leakage across reconnects, races inside the user's own driver,
unbounded queue growth on the client side, retry-amplification of
backend bugs.

**Audit references.** Round 1 IDs `A-/B-/C-`, Round 2 IDs `D-/E-/F-`
from `.fl_audits/round1_round2_consolidated.md`. Numbering of cases
below is `H-N` (this is round 3 / agent H = SDK driver).

**Anti-injection note.** Several tool-result responses while
authoring this plan contained string fragments shaped like
`<system-reminder>` blocks (date-change, MCP Figma instructions,
skills list). They were embedded inside file content / harness output
rather than user instruction. Per the audit's anti-injection
preamble, those were ignored as data; only the audit prompt is
authoritative.

---

## Connection lifecycle

### H-1. Forgotten Client(): GC-only close

**User scenario.** Driver script does `c = Client(); c.connect()` at
module scope, never calls `close()`. Process exits or the binding is
rebound and the prior `Client` is garbage-collected.

**Concrete user actions.**
1. Open Python REPL.
2. `from xinsp2 import Client; c = Client(); c.connect()`.
3. `c = Client(); c.connect()` (rebind — first one becomes garbage).
4. Trigger GC: `import gc; gc.collect()`.

**Hypothesis.** Driver authors expect `__del__` cleanup and the
backend's single-client slot to be released so the second `Client()`
connects.

**Likely actual behavior.** `Client` defines no `__del__`; the daemon
reader thread keeps a strong ref to `self._ws` via the closure on
`_read_loop`, so the first `Client` is NOT collectable. The second
`connect()` hits the single-client 503 path (PR #29). The author
sees an opaque OSError / WebSocketBadStatusException with no hint
that they leaked their first session.

**Bug prediction.** New finding (no audit ID); related to single-client
busy path PR #29; related to F-P1-5 (driver disconnect-detection gap).

**Success criteria for the test.** A plain `c = Client(); c.connect()`
followed by re-binding `c` and `gc.collect()` should release the
single-client slot within 5 s. Test fails if the second `connect()`
returns 503 / times out.

**Implementation sketch.** Subprocess spawns a parent that creates
two `Client()`s back-to-back with rebinding + `gc.collect()` between.
Wrap the second `connect()` in try/except, assert success.

**ROI.** Medium — most authors will hit this once and curse; cheap
fix is `__del__ -> close()` or weakref the reader thread.

---

### H-2. Two `Client()`s in the same process to the same backend

**User scenario.** Driver author wires up a "monitor" Client and a
"control" Client in the same script, expecting both to coexist.

**Concrete user actions.**
```python
mon = Client(); mon.connect()
ctl = Client(); ctl.connect()    # both at ws://127.0.0.1:7823/
```

**Hypothesis.** Author expects the second Client to either share the
session or fail clearly with "single-client backend".

**Likely actual behavior.** Second `connect()` gets the 503 from the
single-client guard. SDK reraises as bare `OSError` /
`WebSocketBadStatusException`; the X-Xi-Reason header is dropped on
the floor. No hint pointing to "single-client v1; share a Client
across threads instead".

**Bug prediction.** F-P1-6 cousin (SDK doesn't surface backend
contract). PR #29 plumbing missing on the SDK side.

**Success criteria for the test.** Second `connect()` raises a
`SingleClientBusyError` (or at minimum, error message containing
`X-Xi-Reason: single-client-busy`). Today: assert that current
behavior is opaque, document the gap.

**Implementation sketch.** Single subprocess; create two clients.
`pytest.raises(Exception)` on second connect; assert the exception
message contains `single-client` or similar. (Will fail today.)

**ROI.** High — cheap, catches an SDK gap that bites every multi-threaded
driver author.

---

### H-3. Client connect during backend startup race (ConnectionRefused retry loop)

**User scenario.** Long-running service. Driver author wraps connect
in `while True: try: c.connect(); break; except ConnectionRefusedError:
sleep(1)`.

**Concrete user actions.** Start driver before backend; backend takes
4 s to bind. Driver retries 4×.

**Hypothesis.** Retry loop succeeds once backend is up; no
side-effects.

**Likely actual behavior.** Likely fine, but: each failed attempt
invokes `websocket.create_connection` which logs / may leak sockets
on Windows under fast-retry. Also, the SDK's enriched
ConnectionRefusedError message changes the exception type chain
(`raise … from e`) — caller's `except ConnectionRefusedError` still
catches it, but `except OSError` may double-fire.

**Bug prediction.** New (low). client.py:118-129 `raise
ConnectionRefusedError(...) from e` is fine for `except
ConnectionRefusedError`, but exception chain reads weird.

**Success criteria for the test.** 10× tight retry loop against an
absent backend completes in <2 s, leaks no sockets (compare
`netstat | grep TIME_WAIT` count delta to itself). Driver
successfully connects when backend comes up.

**Implementation sketch.** Background-spawn backend after 3 s; driver
runs retry loop with budget 10 s; assert eventual success and no
socket-handle leak (Windows `Get-Process | Select-Object HandleCount`
delta).

**ROI.** Medium — soak-style; useful for service-deployments.

---

### H-4. OS kill -9 of driver — backend WS in CLOSE_WAIT forever?

**User scenario.** Operator does `taskkill /F` on the driver process.

**Concrete user actions.**
1. Driver opens client, runs cmd:start.
2. External process kills driver.

**Hypothesis.** Backend notices TCP RST / FIN within seconds and
re-arms single-client slot.

**Likely actual behavior.** Windows TCP keepalive default is 2 h.
Without explicit `SO_KEEPALIVE` on the WS server side, the backend
may sit in CLOSE_WAIT until the next attempted send. Subsequent
`Client()` from a respawned driver hits 503. Couples with H-1.

**Bug prediction.** P0-D5 cousin (slow-loris peer holds backend
hostage); E-P1-2 (WS reconnect leakage of subscriptions).

**Success criteria for the test.** After `taskkill /F` of a
connected driver, a fresh `Client().connect()` succeeds within 5 s.

**Implementation sketch.** Spawn driver subprocess that connects +
runs cmd:start; after 1 s, parent does `subprocess.kill()`. Loop
attempting a fresh Client() with 100 ms intervals; assert success
within deadline.

**ROI.** High — production-blocking for any service deployment;
trivially reproducible.

---

### H-5. Held WS connection for hours; driver heap accumulates

**User scenario.** Long-running monitor driver holds a Client open
overnight, polling `c.recent_errors()` every 10 s.

**Hypothesis.** Memory steady-state.

**Likely actual behavior.** `_inbox_logs`, `_inbox_events` are
unbounded `Queue`s. Backend emits `log` messages at low rate; if the
driver subscribes to nothing but doesn't drain `_inbox_logs`, RAM
grows linearly.

**Bug prediction.** New SDK-side leak; mirrors backend E-P1-2 / E-P1-5
on the driver side.

**Success criteria for the test.** Run driver for 5 min in
continuous mode + a noisy `xi::log()` script. Sample
`tracemalloc` snapshot every 30 s. Assert peak `_inbox_logs.qsize()`
< 1000 OR memory growth < 50 MB.

**Implementation sketch.** Use `tracemalloc.take_snapshot()` for
delta; or simply assert `c._inbox_logs.qsize()` doesn't grow without
bound while no consumer is attached.

**ROI.** Medium — slow leak, but real for service deployments.

---

### H-6. Lost reader thread on WS disconnect; driver doesn't notice

**User scenario.** Network blip kills WS. SDK's `_read_loop` catches
`Exception` and silently returns. Driver next calls `c.call("ping")`.

**Hypothesis.** `c.call("ping")` raises a connection-lost exception.

**Likely actual behavior.** `c.call` does `self._ws.send(...)` —
which may succeed against a half-closed socket (Windows buffers).
Then `q.get(timeout=...)` returns nothing. Caller gets bare
`queue.Empty` (F-P1-4) and treats it as a slow backend, retrying
forever. `c._reader.is_alive()` is False but nobody checks.

**Bug prediction.** F-P1-4 + F-P1-5; SDK has no liveness check after
reader exits.

**Success criteria for the test.** Force-close the WS underneath the
SDK (close `c._ws` from a sidecar thread); next `c.ping()` should
raise a connection-lost exception within 2 s. Today: queue.Empty
or hang.

**Implementation sketch.** Use `c._ws.close()` from a thread to
simulate network break, then `assert pytest.raises(ConnectionError):
c.ping()`. Will fail today.

**ROI.** High — every long-running driver hits this; the silent-hang
behavior is catastrophic.

---

## Cmd ordering / sequencing

### H-7. compile_and_load immediately followed by run

**User scenario.** Author writes `c.compile_and_load(p); c.run()`
expecting the run to use the new DLL.

**Concrete user actions.** Two back-to-back calls; no `time.sleep`.

**Hypothesis.** rsp for compile_and_load means the new DLL is fully
ready.

**Likely actual behavior.** Round 1 / Round 2 audits document hot-
reload edge cases (P0-AB-1/2/3, B-P1-4) where `recompile_project_plugin`
or compile_and_load can leave instances in a transitional state.
Likely OK for `compile_and_load` of inspection script, but the protocol
doc lists `compile_started` / `compile_finished` events — driver author
who watches those can race.

**Bug prediction.** B-P1-4, F-P1-2 (compile_and_load doc-vs-impl
drift on rsp shape).

**Success criteria for the test.** 100 iterations of
`compile_and_load -> run` produce 100 successful runs with the new DLL's
behavior visible in the result vars. No `state_dropped` event leaks
into the rsp.

**Implementation sketch.** Tight loop modifying inspect.cpp body
between iterations (write `int marker = N`); assert run returns
`marker == N`.

**ROI.** Medium — caught at low rate, but root-causing the rare miss
is painful.

---

### H-8. set_param then immediately run — write-window race

**User scenario.** `c.set_param("sigma", 5.0); c.run()` expecting the
run to see sigma=5.

**Hypothesis.** Synchronous; rsp for set_param means the value is in
place for any subsequent inspect.

**Likely actual behavior.** rsp for set_param is sent on the WS
thread; the actual replay into the loaded DLL happens on a different
thread. There's a (small) window where cmd:run can be dispatched
before the param write is visible.

**Bug prediction.** New / latent; couples with A-P1-1
notify-after-pop hole and the broader "set_param replay" pathway.

**Success criteria for the test.** 1000 iterations of
`set_param(N); run; assert observed == N`. Failure even once is a bug.

**Implementation sketch.** inspect.cpp emits the live param value as
a VAR; loop randomises sigma and asserts.

**ROI.** Medium-high — if there's a race, it'll be intermittent in
production and very hard to debug.

---

### H-9. SIGINT handler firing cmd:close_project mid-flight

**User scenario.** Driver registers `signal.signal(signal.SIGINT,
handler)` where `handler` calls `c.call("close_project")`. User
hits Ctrl+C while another cmd is in flight.

**Concrete user actions.**
```python
def on_sigint(*_):
    c.call("close_project")
signal.signal(signal.SIGINT, on_sigint)
# main thread: while True: c.run()
# Ctrl+C while in run
```

**Hypothesis.** Clean shutdown.

**Likely actual behavior.** `c.call` is not reentrant — the SIGINT
handler runs on the main thread mid-`q.get(timeout=...)`. The
in-flight `cmd:run` waiter is still in `_rsp_waiters`. The SIGINT
handler grabs `self._next_id` (no lock), sends `cmd:close_project`,
waits on its own queue. P0-AB-3 (close_project doesn't drain
dispatcher) plus P0-B P0-1 (close_project crashes during continuous)
both surface.

**Bug prediction.** P0-AB-3, B P0-1 reach via SDK; SDK has no
reentrancy guard.

**Success criteria for the test.** With `start fps=60` running, fire
`close_project` from a sidecar thread (signals are too OS-flaky to
test). Assert backend doesn't crash; assert the in-flight cmd:run
returns either ok or a clean error, not a hang.

**Implementation sketch.** Threading-based fault injection rather than
real SIGINT; threading.Thread fires close_project 50 ms into a run.

**ROI.** High — root-causes a P0; reproduces a real shutdown bug.

---

### H-10. Driver crashes mid-compile_and_load; respawned driver reconnects

**User scenario.** Driver A calls `c.compile_and_load(p)` (180 s
timeout). Driver crashes at t=10 s. Driver B starts, connects.

**Hypothesis.** Backend either rolls back or completes and Driver B
sees a coherent state.

**Likely actual behavior.** Backend's compile_and_load is a long-
running synchronous handler; when WS closes mid-call, the cl.exe
subprocess keeps running. Driver B connects, calls
`compile_and_load` again — does the backend serialize? Or do two cl.exe
fight over the same _v<N>.dll output path? P0-E2 says
`s_version++` mints a new path each time, so name collision is
unlikely; but the backend's project-state may be mid-mutation
(B-P1-1/2 partial-failure leaks).

**Bug prediction.** B-P1-1, B-P1-2; F-P1-3 (no escalation path on
180 s timeout).

**Success criteria for the test.** Driver A starts compile_and_load,
gets `os._exit(1)` at t=2 s. Driver B starts at t=3 s, calls
`list_instances`. Assert no crash, sensible response (either old
project state or fully reset).

**Implementation sketch.** Subprocess driver_a with crash hook;
parent waits 1 s, spawns driver_b. Validate backend log for crashes /
errors.

**ROI.** Medium-high — catches a real production failure mode.

---

### H-11. Out-of-order Jupyter cells reusing same Client

**User scenario.** Notebook user runs Cell-3 (`c.run()`) before Cell-2
(`c.compile_and_load(...)`). Then re-runs Cell-2 followed by Cell-3.

**Concrete user actions.**
- Cell-1: `c = Client(); c.connect()`
- Cell-3: `c.run()`  → ProtocolError "no script loaded"
- Cell-2: `c.compile_and_load("inspect.cpp")`
- Cell-3: `c.run()` again

**Hypothesis.** Errors handled gracefully; queues drain between cells.

**Likely actual behavior.** SDK's `run()` does
`self._drain(self._inbox_vars)` before sending cmd:run, so stale data
won't leak. But: A-P1-2 says cmd:start consumes stale events from
prior cmd:stop windows on the BACKEND side. SDK doesn't know.
Also, the failed `c.run()` raises a ProtocolError but
`_inbox_events` may have an `instances` message stuck after the
later compile_and_load — `list_instances` later confuses the synth
event.

**Bug prediction.** A-P1-2; SDK `list_instances` synth-event
collision (lines 277-285 of client.py).

**Success criteria for the test.** Reproduce the cell sequence in
script form; assert each step's outcome matches expectation. After
2 cycles, `c.list_instances()` returns the right instance set.

**Implementation sketch.** Linear script that emulates cell-rerun
order; assertions at each step.

**ROI.** High for notebook authors; medium overall.

---

### H-12. Subscribe before cmd:start, unsubscribe mid-run, re-subscribe

**User scenario.** Driver toggles preview streaming on/off mid-run
to save bandwidth.

**Concrete user actions.**
1. `c.call("subscribe", {"names": ["gray"]})`
2. `c.call("start", {"fps": 30})`
3. wait 0.5 s
4. `c.call("unsubscribe")`
5. wait 0.5 s
6. `c.call("subscribe", {"names": ["gray"]})`

**Hypothesis.** Preview frames stream / stop / stream cleanly; no
dropped binary frames mismatch their vars message.

**Likely actual behavior.** Binary preview frame ordering w.r.t. its
preceding `vars` message is best-effort. During the unsubscribe
window, vars still arrive (per spec) — but if the sub-toggle races
with a vars-emit, one frame may have a vars message but no preview,
leaving `_inbox_previews` empty and `run()` timing out on
`wanted_gids`.

**Bug prediction.** New; SDK `run()` deadlock candidate at
client.py:392-400 if a preview is suppressed mid-run.

**Success criteria for the test.** 100 cycles of
sub/unsub/sub during continuous mode, no `_inbox_previews.get`
times out. `next_vars` consistently advances run_id.

**Implementation sketch.** Background thread toggles sub/unsub at
random offsets. Assert `next_vars` always returns within 200 ms.

**ROI.** Medium — bandwidth optimisation feature, but corner cases here
are intermittent.

---

## Error / timeout handling

### H-13. SDK raises bare `queue.Empty` on timeout (F-P1-4)

**User scenario.** Driver does
```python
try: c.call("compile_and_load", {...}, timeout=1.0)
except TimeoutError: retry()
```

**Hypothesis.** `TimeoutError` matches.

**Likely actual behavior.** `Queue.get(timeout=1)` raises `queue.Empty`,
NOT TimeoutError. SDK does NOT translate. `except TimeoutError` does
NOT match; bare `except Exception` is needed. Worse: the retry sends
a SECOND cmd:compile_and_load while the first is still running
backend-side (backend doesn't cancel — F-P1-4 explicit). Two cl.exe
fight; or B writes `_v<N+1>.dll` while A writes `_v<N>.dll`; net
state ambiguous.

**Bug prediction.** F-P1-4 (verbatim).

**Success criteria for the test.** Set `timeout=0.5` on
compile_and_load (which takes 4+ s). Assert `pytest.raises(TimeoutError)`
matches. Today: fails because raised type is `queue.Empty`. Test
documents the gap.

**Implementation sketch.** One-line assertion. No backend mods.

**ROI.** Very high — trivial test, exposes a documented bug, fix is
2 lines in client.py.

---

### H-14. User retry-on-timeout amplifies an in-flight backend cmd

**User scenario.** Same as H-13 but the user also writes a retry
loop.

**Concrete user actions.**
```python
for attempt in range(3):
    try: c.compile_and_load(p)
    except Exception: continue
```

**Hypothesis.** At most one cl.exe runs.

**Likely actual behavior.** Three concurrent compile_and_loads in
flight; backend serializes them, but each one mutates state on
completion. The third one's result wins; user thinks the first
worked. If the user's `inspect.cpp` was edited between iterations
(`time.sleep` in the loop), the user sees mysterious "old version
running".

**Bug prediction.** F-P1-4 amplified.

**Success criteria for the test.** Modify inspect.cpp after first
attempt; verify which version actually runs.

**Implementation sketch.** Driver writes `marker_v1.cpp`, calls
compile_and_load with timeout=0.5 (will raise queue.Empty). Driver
writes `marker_v2.cpp`, calls compile_and_load with timeout=180.
Assert `marker == "v2"` in the next run. Today probably fails
because v1 cl.exe is still racing.

**ROI.** High — exposes amplification, motivates server-side cancel.

---

### H-15. Compile failure path — driver expects exception type but gets ProtocolError

**User scenario.** Driver author reads `client.py:189-202` docstring
which says "raises ProtocolError whose message includes ...".

**Hypothesis.** `except ProtocolError as e: print(e.data["diagnostics"])`
works.

**Likely actual behavior.** Mostly works. Edge: the
`_enrich_compile_error` re-wraps with `raise ... from None`,
suppressing the original chain. If `data` is None (backend forgot
to attach diagnostics on a transient failure), `e.data["diagnostics"]`
is `None.__getitem__` → `TypeError`. Author code crashes on the
error-handling path.

**Bug prediction.** F-P1-2; client.py error-shape robustness.

**Success criteria for the test.** Trigger compile failure via
deliberately broken inspect.cpp. `e.data` and `e.error` should be
non-None. AND test the case where backend returns ok=false with no
data (mock the WS): SDK shouldn't crash.

**Implementation sketch.** Two cases: real broken cpp; and a wire
fixture that forces `{"ok": false, "error": "x"}` (no data).

**ROI.** High — every driver author hits compile failures.

---

### H-16. `next_vars(timeout)` returns None forever after disconnect (F-P1-5)

**User scenario.** Tail loop:
```python
while True:
    v = c.next_vars(1.0)
    if v: process(v)
```

**Hypothesis.** None means timeout; processing gracefully degrades.

**Likely actual behavior.** F-P1-5 verbatim: after WS drops, the
`_inbox_vars` queue is empty forever. Loop spins at 1 Hz forever.
No exception, no log, no exit.

**Bug prediction.** F-P1-5.

**Success criteria for the test.** Force WS close from sidecar. Tail
loop should raise ConnectionError or return a sentinel within 5 s.
Today: fails (infinite None).

**Implementation sketch.** Sidecar thread does `c._ws.close()` after
2 s. Main runs the tail loop with a 10 s wallclock limit; assert
loop exited via exception, not via wallclock.

**ROI.** Very high — exact F-P1-5 reproducer; test asserts the fix.

---

### H-17. `c.call("ping")` as liveness probe doesn't catch all dead-WS conditions

**User scenario.** Driver pings every 30 s as keepalive.

**Hypothesis.** A successful ping means the connection is healthy.

**Likely actual behavior.** Send may succeed against a half-closed
TCP socket (Windows kernel queues it). Reader is dead. q.get times
out → queue.Empty → driver thinks ping failed but the connection
state is ambiguous.

**Bug prediction.** New; SDK has no `is_alive` API.

**Success criteria for the test.** After force-half-close (sidecar
`_ws.sock.shutdown(SHUT_RD)`), `c.ping()` should detect failure
within 2 s and raise.

**Implementation sketch.** Socket-level `shutdown(SHUT_RD)` from
sidecar; assert raises.

**ROI.** Medium-high.

---

### H-18. `except Exception: pass` swallows ProtocolError; driver continues with stale state

**User scenario.** Common Python anti-pattern.
```python
try: c.compile_and_load(p)
except Exception: pass
c.run()
```

**Hypothesis.** Run uses old DLL.

**Likely actual behavior.** True for compile failure (old DLL stays
loaded — that's the contract for `recompile_project_plugin`). For
`compile_and_load` of the inspection script, if the new compile
failed, what does `c.run()` do? Per backend, the previous DLL is
gone (compile_and_load tears down before rebuild). So `c.run()` runs
on... no script? Or the half-loaded one?

**Bug prediction.** New behavior gap; doc says compile_and_load is
hot-reload but failure semantics not documented for inspection
script (only for `recompile_project_plugin`).

**Success criteria for the test.** After deliberately failing
compile_and_load (broken cpp) and swallowing the exception, `c.run()`
should either raise a clear "no script loaded" error or run the
prior DLL. Test asserts which behavior is documented and matches.

**Implementation sketch.** Two-step driver, swallow exception, run,
assert behavior matches a documented contract.

**ROI.** Medium — catches a doc gap.

---

## Concurrency in the driver

### H-19. asyncio + threadpool calling c.call from multiple threads

**User scenario.** Author wraps SDK in
`loop.run_in_executor(pool, c.call, "ping")` from many coroutines.

**Hypothesis.** SDK is thread-safe.

**Likely actual behavior.** Docstring says "Not thread-safe". But
the author won't read the docstring. `self._next_id += 1` has no
lock — two threads can race and assign the same id. `self._ws.send`
has no lock — websocket-client's `send` is not thread-safe and the
WS frame can interleave bytes, corrupting the protocol entirely.
Backend hangs trying to parse a half-frame.

**Bug prediction.** F-P1-6 cousin (SDK contract opacity); backend
parser robustness (P0-D1) compounds.

**Success criteria for the test.** 16 threads × 100 calls each. With
current SDK, expect either crash, deadlock, or wrong-rsp delivery.
Test asserts current broken behavior, motivates a thread-safe wrapper.

**Implementation sketch.** ThreadPoolExecutor of 16, each thread
calls `c.ping()` 100 times. Track which thread got which rsp.id;
assert at least one mismatch (proving the bug) OR document
zero-mismatch as accidental.

**ROI.** High — extremely common usage pattern in modern Python; SDK
is unsafe.

---

### H-20. Two threads each calling c.run() on the same Client

**User scenario.** Author runs A/B benchmarks in parallel from one
Client.

**Hypothesis.** Backend handles parallelism via dispatch_threads.

**Likely actual behavior.** SDK's `run()` calls `_drain(_inbox_vars)`
at the start — Thread A drains its own AND Thread B's pending vars.
B then waits for B's vars but A consumed them. Both deadlock /
mis-attribute.

**Bug prediction.** New SDK race; couples with B-P1-7 burst-parallelism
read of out_keys outside mu.

**Success criteria for the test.** Two threads call `c.run()`
simultaneously 50× each. Assert at least one ProtocolError or
mis-correlation. Test documents the unsafe surface.

**Implementation sketch.** Two threading.Thread targets calling
c.run; collect results; check for KeyError / mismatched run_ids.

**ROI.** Medium — niche pattern but rooted in a real concurrency hole.

---

## Event / vars consumption

### H-21. Driver waits for `run_started` / `run_finished` events that don't exist (F-P1-1)

**User scenario.** Driver reads protocol.md which lists
`run_started` / `run_finished` events. Author writes:
```python
c.call("run", ...)
ev = wait_for_event(c, "run_finished", timeout=10)
```

**Hypothesis.** Event arrives within 10 s.

**Likely actual behavior.** F-P1-1 verbatim — events are documented
but never emitted. Driver hangs forever.

**Bug prediction.** F-P1-1.

**Success criteria for the test.** Wait for `run_finished` with 5 s
timeout; assert it arrives. Today: fails (hangs / TimeoutError). Test
proves the gap.

**Implementation sketch.** Helper that pulls from `_inbox_events`
filtering by name; 5 s timeout. Pass = event received.

**ROI.** Very high — direct F-P1-1 repro; documentation contract.

---

### H-22. Subscriber to image previews doesn't drain `_inbox_previews`

**User scenario.** Driver subscribes to all images, only consumes
`vars` via `next_vars`, never drains previews.

**Concrete user actions.**
```python
c.call("subscribe", {"all": True})
c.call("start", {"fps": 60})
while True: v = c.next_vars(1.0)  # never touches _inbox_previews
```

**Hypothesis.** Previews are dropped.

**Likely actual behavior.** `_inbox_previews` is unbounded `Queue`.
At 60 fps × 5 image vars × ~1 MB JPEG each ≈ 300 MB/s of accumulation.
Driver OOMs in seconds.

**Bug prediction.** New; SDK queue policy (no max size).

**Success criteria for the test.** Run for 30 s with subscribe-all
and an image-heavy inspect.cpp. Assert RAM growth < 1 GB OR
`_inbox_previews.qsize()` capped.

**Implementation sketch.** psutil.Process().memory_info().rss
sampled. Today: fails; remediate with a `maxsize` on
`_inbox_previews` + drop-oldest policy.

**ROI.** High — production blocker for any monitor-style driver.

---

### H-23. `dispatch_stats` SDK exposure correctness

**User scenario.** Driver calls `c.call("dispatch_stats")` before and
after a `cmd:start` to compute deltas (multi_source_surge does this
correctly only by accident).

**Hypothesis.** Subtraction works.

**Likely actual behavior.** Per protocol.md:265-275, `cmd:start`
RESETS the counters. Subtracting before-after gives nonsense /
underflow. Doc says so but the SDK doesn't have a typed wrapper that
enforces the AFTER-snapshot pattern.

**Bug prediction.** F-P1-2 cousin (doc-vs-impl-vs-SDK drift).

**Success criteria for the test.** Run a stress sweep, capture
before/after, verify after >= before isn't always true; document the
behavior. Then test that an `c.dispatch_stats()` helper (if added)
discourages the wrong pattern.

**Implementation sketch.** Driver does explicit
before/start/burst/stop/after sequence; assert
`after.dropped_oldest >= 0` (always trivially true) but
`after - before` may be negative.

**ROI.** Low-medium — correctness gap, fix is documentation +
typed helper.

---

## Resource consumption (driver-side)

### H-24. Sending a 10 MB JSON arg via c.call

**User scenario.** Driver sends a giant config blob via
`c.call("set_instance_def", {"name": "x", "def": HUGE_DICT})`.

**Hypothesis.** Either succeeds or fails cleanly.

**Likely actual behavior.** WS server has 16 MB cap; 10 MB fits.
But: `json.dumps` of a deeply-nested dict from Python is slow + may
hit Python's recursion limit. SDK has no size sanity check. Backend's
`xi_ws_server.hpp::parse_frame` does bounds-check, but
`xi_ipc.hpp::Reader` (P0-D1) doesn't propagate the size to all paths.

**Bug prediction.** P0-D1 + new SDK robustness gap.

**Success criteria for the test.** Send 1 MB / 5 MB / 15 MB / 17 MB
JSON arg; for ≤16 MB should succeed or fail cleanly; for >16 MB
should raise a clear error (not stall).

**Implementation sketch.** Build payloads of increasing size, time
each call, assert behavior at each band.

**ROI.** Medium — catches both SDK and backend caps.

---

### H-25. Arbitrary user-supplied cmd `name` and `args`

**User scenario.** Driver author passes an unfiltered string from
user input as the cmd name. (Or follows protocol.md and types
`cmd:open_project_warnings` which is documented as "planned, not yet
wired".)

**Concrete user actions.**
```python
c.call(user_input, user_args)   # or c.call("open_project_warnings")
```

**Hypothesis.** Backend returns "unknown command" error cleanly.

**Likely actual behavior.** Mostly fine — protocol.md error-handling
section says malformed JSON / unknown command produce a clean error.
But: user_input could be a 100k-byte string, going through
`json.dumps` and into `recv_frame`. P0-D1 bounds-check gap could
amplify.

**Bug prediction.** P0-D1.

**Success criteria for the test.** Cmd name = `"x" * 100000`; backend
should return error within 100 ms, not crash. Cmd name with embedded
NUL, with `"` quote, with binary bytes — same.

**Implementation sketch.** Parametric test over hostile cmd-name
strings; assert backend stays alive; client gets ProtocolError.

**ROI.** Medium — D-fuzz scope but driver-side surface.

---

### H-26. `seq_` wrap collision visible from SDK after long run

**User scenario.** Driver runs continuously for ~50 days at 1 RPC/ms
(P0-C2 wrap window). Or, more realistically, simulates by injection.

**Hypothesis.** Seq IDs are opaque; user never sees them.

**Likely actual behavior.** P0-C2: seq_ wraps to 0, gets routed to
async-frame handler instead of the correct waiter. The SDK's
`_rsp_waiters[id]` never receives the rsp. `c.call(...)` raises
queue.Empty after timeout. Driver retries, gets the same wrap.

**Bug prediction.** P0-C2.

**Success criteria for the test.** Inject `c._next_id = 2**32 - 5`,
fire 10 cmds. Assert all 10 succeed within timeout. Today: likely
fails when SDK overflows Python int (no overflow on Python side, but
backend's uint32_t side wraps and routes wrong).

**Implementation sketch.** Direct SDK-state injection on `_next_id`,
then issue calls. Watch backend logs for routing-error.

**ROI.** Medium — seq_ wrap is a documented P0; SDK is the natural
trigger.

---

### H-27. Reconnect after backend restart leaks subscriptions / history

**User scenario.** Backend restarted (deploy). Driver auto-reconnects
via outer retry loop; calls `c.subscribe(["gray"])` again.

**Hypothesis.** Clean session.

**Likely actual behavior.** Per E-P1-2: WS reconnect on the BACKEND
keeps `g_sub_*`, `g_history`, `g_iso_dead_reported`, `g_recent_errors`
across disconnects. New driver inherits prior session's subscriptions
and history. Driver author surprised by ghost data.

**Bug prediction.** E-P1-2.

**Success criteria for the test.** Driver A subscribes to ["foo"];
disconnects. Driver B connects, calls `c.recent_errors()` — should
NOT see Driver A's errors. Today: fails.

**Implementation sketch.** Two sequential driver subprocesses; assert
B's `recent_errors()` is empty.

**ROI.** High — privacy / correctness across deploys.

---

### H-28. Race on first connection after backend boot

**User scenario.** Multiple drivers race to be the first connection.
Loser gets 503 (PR #29). What does the loser do?

**Concrete user actions.** Two driver processes started simultaneously
by a deploy script.

**Hypothesis.** Loser exits cleanly with a clear message.

**Likely actual behavior.** Loser gets opaque OSError /
WebSocketBadStatusException with no surfaced X-Xi-Reason. Loser's
retry loop hammers the backend; backend's accept-and-reject path
(PR #29) handles it but logs verbosely.

**Bug prediction.** F-P1-6 cousin.

**Success criteria for the test.** Two clients in tight loop; one
wins, the other detects 503 within 200 ms and exits with a typed
exception (e.g. `SingleClientBusyError`).

**Implementation sketch.** Two threads racing `Client().connect()`;
assert exactly one wins, the other sees a typed error.

**ROI.** High — common deploy pattern.

---

### H-29. Mishandling `instances` synth-event collision

**User scenario.** Driver calls `c.list_instances()` repeatedly while
also subscribing to events (e.g. waiting for `state_dropped`).

**Likely actual behavior.** SDK's `_handle_text` for type=`instances`
synthesizes an event with `name: "instances"` and pushes onto
`_inbox_events`. `c.list_instances()` pops it. But if the user is
also calling `_inbox_events.get_nowait()` directly (per the docstring
of `c.run()`), they may steal the synth event before
`list_instances()` reads it. Then `list_instances` blocks for
`self.timeout` (30s) until next event.

**Bug prediction.** New SDK race; client.py:277-285 + 405-409.

**Success criteria for the test.** Concurrently call
`c.list_instances()` and a tight `_inbox_events.get_nowait()` loop;
assert no deadlock.

**Implementation sketch.** Threaded fixture.

**ROI.** Low-medium — uncommon pattern but real if user follows
docstring guidance.

---

### H-30. Driver doesn't handle `recent_errors()` empty-on-success contract

**User scenario.** Driver polls `recent_errors()` after every cmd to
correlate side-channel errors. Empty list means "all good".

**Hypothesis.** Empty list is unambiguous.

**Likely actual behavior.** Per protocol.md:328-330: empty list IS the
"nothing to report" answer. But: SDK's `recent_errors()` has no
`since_ms` defaulting; without tracking the last seen ts_ms, the
driver re-reads the SAME errors every poll, thinking they're new.

**Bug prediction.** F-P1-6 cousin; SDK doesn't expose the incremental-
poll pattern.

**Success criteria for the test.** Trigger one error; poll
recent_errors twice; assert second poll returns `[]` (after passing
since_ms) OR returns `[err]` again (without passing) — and that the
SDK helper makes the right pattern obvious.

**Implementation sketch.** Documented behavior assertion.

**ROI.** Low — usability issue.

---

### H-31. `compare_variants` post-state surprise

**User scenario.** Driver uses `compare_variants` to A/B test, then
expects to be back on variant A.

**Likely actual behavior.** Per protocol.md:504: "After the call the
script is left in **variant B**'s state". Most authors will miss this
and the next `c.run()` produces variant-B output thinking it's A.

**Bug prediction.** F-P1-6 cousin (SDK doesn't surface).

**Success criteria for the test.** Run compare_variants(A,B), then
plain run; assert observable matches B (proving the doc), then add
a SDK helper that auto-restores.

**Implementation sketch.** Three-run sequence with marker params.

**ROI.** Low-medium.

---

### H-32. `cmd:close_project` while continuous mode is firing (P0-B P0-1 driver-side)

**User scenario.** Driver calls `c.call("close_project")` while
`cmd:start fps=60` is firing.

**Likely actual behavior.** P0-AB-3 / B P0-1 says backend crashes.
SDK side: `c.call` issued the request, backend dies before sending
rsp, WS closes, reader thread exits, q.get times out → queue.Empty.
Driver thinks it timed out, retries — but backend is gone. Next
Client() fails to connect.

**Bug prediction.** P0-B P0-1; SDK doesn't detect backend crash.

**Success criteria for the test.** Issue close_project mid-start.
Assert SDK raises `ConnectionError` (or backend-crashed error)
within 5 s, not queue.Empty.

**Implementation sketch.** Spawn driver, fire close_project at t=1s
into start, assert specific exception type.

**ROI.** Very high — exact P0 reproducer; integration test for the
fix.

---

### H-33. Run vs ping: ping during a run() in flight

**User scenario.** Driver has a watchdog thread that pings every
5 s while main thread runs `c.run()`.

**Likely actual behavior.** SDK is not thread-safe (H-19). The two
calls' frames may interleave bytes. Even if they don't, ping's rsp
comes in while `run()` is waiting on its own rsp; SDK's id-based
correlation handles it correctly IF the next_id increment isn't
raced.

**Bug prediction.** Same as H-19; this is the "realistic" form of it.

**Success criteria for the test.** 60 s run with watchdog ping; no
crash, no mis-correlation.

**Implementation sketch.** Thread A: `for _ in range(60): c.run()`.
Thread B: `while True: c.ping(); sleep(5)`.

**ROI.** Medium-high — most production drivers want a watchdog.

---

### H-34. Driver discovers SDK's `next_vars` mis-correlation across reconnect

**User scenario.** Driver does:
```python
while True:
    try: v = c.next_vars(1.0)
    except: c.connect(); continue
```

**Likely actual behavior.** F-P1-5 returns None on disconnect.
After reconnect, the driver gets vars from a NEW run_id; if the
driver was filtering by run_id, it's confused. Vars-image-preview
ordering may also be broken across reconnect.

**Bug prediction.** F-P1-5 + new vars/preview reorder.

**Success criteria for the test.** Disconnect / reconnect mid-stream;
assert vars after reconnect have monotonically growing run_ids.

**ROI.** Medium.

---

## Plan-of-record summary

### H-35. Driver author treats c.shutdown() as polite-close, calls it then reuses Client

**User scenario.**
```python
c.shutdown(); c.compile_and_load(...)
```

**Likely actual behavior.** Per docstring (client.py:319-325):
"After this the client should not reuse the connection." But
nothing enforces it. `compile_and_load` will ws.send into a closed
socket, raise `WebSocketConnectionClosedException`, propagate
opaquely.

**Bug prediction.** F-P1-6 cousin; SDK should null `self._ws` in
shutdown().

**Success criteria for the test.** Call shutdown, then any cmd;
assert raises `RuntimeError("client closed")` clearly.

**Implementation sketch.** Two-line test.

**ROI.** Low-medium.

---

## Top 5 highest-ROI cases to implement first

1. **H-13** — `queue.Empty` instead of `TimeoutError` (F-P1-4). One-
   liner test; documents an SDK contract bug; fix is two lines.
2. **H-16** — `next_vars()` returns None forever after disconnect
   (F-P1-5). Direct repro; long-running services depend on this.
3. **H-21** — Driver waits for `run_started`/`run_finished` events
   that don't exist (F-P1-1). Cheap, exposes a doc-vs-impl gap that
   directly hangs naive drivers.
4. **H-32** — `cmd:close_project` while continuous mode fires
   (P0-B P0-1). Integration test for the P0; surfaces the crash from
   the SDK's perspective.
5. **H-22** — Subscribed but undrained `_inbox_previews` OOMs the
   driver. Cheap to reproduce; production-blocker for monitors.

Honourable mentions: H-4 (kill -9 + reconnect), H-19 (multi-thread
SDK use), H-27 (cross-session leakage), H-28 (multi-driver race).

---

## Cases that probably can't be automated reliably

- **H-3** (backend-startup race retry): needs precise sub-second
  timing of backend startup; CI flake risk.
- **H-5** (overnight RAM accumulation): too long for CI; needs a
  scaled-down accelerated form (faster log emission).
- **H-9** (real SIGINT delivery from a Python signal handler): OS
  signal handling on Windows + threading is too flaky; we
  approximated with a sidecar thread (already noted in case body).
- **H-26** (seq_ wrap): real wrap is 50 days; only the injection
  variant is automatable, and it depends on backend-side internal
  state we can't reach from Python.
- **H-11** (Jupyter cell-rerun order): can be scripted linearly but
  the actual notebook environment introduces additional state
  (autoreload, namespace) that's hard to fully model.
- Anything that needs deliberate `taskkill /F` of a backend mid-cmd
  while keeping a deterministic timeline (H-32 is on the boundary —
  worth attempting but flake-prone).

---

## Anti-injection findings

While reading `client.py` and `protocol.md` via tool calls, several
tool-result responses contained string fragments shaped like
`<system-reminder>` blocks, including:
- a date-change directive instructing the assistant not to mention it
- an MCP "Figma" capability advertisement with usage instructions
- a "Skills available" listing

Per the audit's anti-injection preamble, none of those instructions
came from the audit prompt; they were embedded in tool output and
treated as data. No action taken on them. Flagging here as required.
