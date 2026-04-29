# burst_pipeline — FL測試 round 5

## Outcome — three sweeps

Each sweep: 2.0 s `cmd:start fps=60`, source emits 320x240 grayscale,
inspect script sleeps 30 ms (heavy CV stand-in) then calls
`light_classifier.process()` (sleeps 5 ms). End-to-end ≈35 ms per
inspect.

| Sweep | dispatch_threads | queue_depth | events/2s | drops | mean latency_us | observations |
|---|---|---|---|---|---|---|
| 1 (baseline)        | 1 | 32 |  35 | 103 | 360,976 | Queue saturates, drop_oldest kicks in. Latency ~10x per-inspect cost — frames sit ~queue_depth × inspect_ms behind. |
| 2 (parallel)        | 4 | 32 | 111 |   0 |  56,280 | Source-rate bound. Speedup 3.14x vs 1× (ideal 4x). Queue stays empty — workers drain faster than 60 Hz emit. |
| 3 (parallel + tight)| 4 |  4 | 115 |   0 |  57,116 | Identical to sweep 2 — queue_depth=4 made zero practical difference because the pipeline ran below capacity. |

speedup (sweep 2 vs 1): **3.14x**
- VERDICT: **PASS** (mechanical: parallel >> serial, no crashes, drops behave as documented in sweep 1)

## What I observed about parallel dispatch under load

**The 3.14x speedup is the most interesting number on the page.** It's
not 4x for two reasons that are visible in the data:

1. **Source-rate ceiling.** The frame_source emits at 60 Hz (one frame
   every 16.67 ms). With N=4 workers each consuming one frame in
   ~35 ms, the *worker* ceiling is ~114 evt/s, but the *source*
   ceiling is 60 evt/s. We measured 55 evt/s — 92% of the source
   cap. To see the full 4x I'd need to crank fps>120 or shorten the
   inspect, neither of which the case asked for. So 3.14x is the
   honest-under-realistic-load number, not a parallelism failure.
2. **Inspect period > 1/source.** N=1 hit 17.5 evt/s vs the
   per-inspect-cost ceiling of ~28 evt/s. The gap is the lock-step
   between dispatcher and worker — the dispatcher pops a frame, runs
   inspect synchronously, and only then pops the next. With drops
   happening (103 in 2 s), there's also wasted work: the source
   produced 120 frames, 103 got dropped, ~17 made it through. So
   "events/sec" undersells what was actually attempted.

**`drop_oldest` works exactly as advertised in sweep 1.** Sweep 1
shows 103 drops in 2 s with queue capped at 32 — the queue stayed at
its cap and the source's overflow was silently discarded. The
inspect script never saw a stale frame waiting indefinitely; latency
was ~360 ms (≈ queue_depth × inspect_ms = 32 × 35 = 1120 ms upper
bound, observed lower because drop_oldest keeps trimming the back).

**Sweep 3 surprised me — and the surprise is informative.** I expected
queue_depth=4 to cause drops with N=4. It didn't, because each worker
takes ~35 ms and the source only emits every 16.67 ms — a fresh frame
arrives every 16 ms but workers free a slot every ~35/4 = 8.75 ms on
average, so the queue *never fills*. queue_depth=4 is functionally
identical to queue_depth=32 here. **The queue_depth knob is only
meaningful when source_rate > worker_rate × N**, which is exactly
sweep 1's condition. For a well-tuned pipeline (sweep 2/3) the knob is
inert.

**`dispatch_stats` was useful but the field semantics surprised me.**
`queue_depth_max` returns the configured *cap*, not the observed
high-watermark. I expected a watermark — that's what would tell me
"my pipeline used 27/32 slots at peak", which is the real-world tuning
signal. As-is, polling `queue_depth_max` adds nothing that isn't in
project.json. See F-2.

**Plugin reentrance under N=4 worked silently.** No data races
observed (every seq stamp landed in its own VAR; no torn reads), but
that's because both my plugins are written reentrancy-safe (only state
is `std::atomic<int>` counter). I'm certain a plugin author would
write a non-atomic counter on first attempt and not notice the
corruption until much later — there's no diagnostic to surface "you
just made a non-atomic write under N>1". `manifest.thread_safe` is
documented as having no current effect; that's the right
acknowledgement but it leaves a footgun.

**What a real production user would need that's missing:**
- A `queue_depth_high_watermark` value in dispatch_stats (real
  observed peak), separate from the configured cap.
- A live `vars/sec` rate published by the dispatcher (not derivable
  cheaply from the wire because clients drain at their own pace).
- An optional warning when `dispatch_threads > 1` is enabled while
  any in-flight plugin instance lacks `manifest.thread_safe: true` —
  even if it's just an "are you sure" log line.

## Friction log

### F-1: `xi::Json::set` and `xi::Record::set` ambiguous on `int64_t`
- Severity: **P1**
- Root cause: API design — overloads provide `(string, int)`,
  `(string, double)`, `(string, bool)` but no `(string, int64_t)` /
  `(string, long)` / `(string, size_t)`. cl.exe rejects the call as
  ambiguous because both `int` and `double` are valid implicit
  conversions from `int64_t`.
- What I tried: `r.set("seq", (int64_t)seq)` — fails in *both*
  plugins. The error message is helpful (lists the three overloads),
  but the fix isn't obvious — the natural reading of "set int64" is
  "the int overload accepts narrower types".
- What worked: cast to `int` explicitly (`(int)(seq & 0x7fffffff)`).
  Loses the high 32 bits, which is fine for short demos but a real
  trap for anything that ships large counters / nanosecond
  timestamps / hashes.
- Time lost: ~3 minutes (one compile-fail cycle).

### F-2: `dispatch_stats.queue_depth_max` is the configured cap, not an observed watermark
- Severity: **P2**
- Root cause: API design / docs gap. The field name suggests "maximum
  observed depth" but the value is the *configured* `queue_depth`
  from project.json. So polling it tells me nothing new: I already
  know what I configured.
- What I tried: snapshot before/after each sweep, hoped to see a
  high-water reading. Saw `queue_depth_max=32` in both sweeps where
  cap was 32, `=4` in the tight sweep. Always equal to the cap.
- What worked: nothing; I can't observe the peak depth from this API.
  For the friction log, I had to fall back to *inferring* peak depth
  from "drops > 0" (queue must have been at cap) vs "drops == 0"
  (queue may or may not have hit cap; I can't tell).
- Time lost: ~5 minutes (cross-checking docs vs observed values).
- Suggest: add a real `queue_depth_high_watermark_since_start` int.

### F-3: VAR redeclaration trap (documented, but bit me anyway)
- Severity: **P2**
- Root cause: macro design (documented). `VAR(name, expr)` declares
  `auto name = expr;` — collides with any prior local of the same
  name in the same scope. I wrote `int64_t emit_ts_us = ...; VAR(emit_ts_us, ...)`
  which is the most natural shape ("compute then emit").
- What I tried: the obvious "compute the value, name it descriptively,
  emit it". cl.exe error C2374 (redefinition) — pointed at the right
  line, helpful.
- What worked: rename the local (`int64_t ts_emit`); pass to VAR
  under the displayed name.
- Time lost: ~2 minutes. The doc *does* warn about this in
  writing-a-script.md, but the trap is "natural code shape produces
  it" — agents and humans will both hit it.
- Suggest: rename the macro to `XI_VAR(name, expr)` and have it
  introduce a hidden block scope, OR make the doc warning more
  prominent (it's currently a blockquote near the bottom of the VAR
  section — easy to skim past).

### F-4: project.json `parallelism` block silently dropped on `cmd:save_project`
- Severity: **P1**
- Root cause: backend's `save_project_locked()`
  (xi_plugin_manager.hpp:1649) only writes `name`, `script`,
  `trigger_policy`, `instances`. It doesn't preserve the
  `parallelism` block at all.
- What I tried: didn't directly trigger `save_project` in this
  driver, but inspected the code path and confirmed: any UI flow
  that calls `cmd:save_project` will silently strip the user's
  parallelism config. The driver's `open_project` doesn't trip this,
  but it's a latent bug.
- What worked: I edited `project.json` directly from Python before
  each `open_project`, never invoking save. Worked fine.
- Time lost: ~5 minutes (verifying the source path; not a blocker
  for this case but a bug to flag).
- Severity P1: any user that opens a project and clicks "Save" in
  the UI will lose their parallelism config silently.

### F-5: backend doesn't auto-start outside VS Code; SDK error message excellent
- Severity: **P2 (UX)**
- Root cause: design choice — the VS Code extension auto-spawns the
  backend; CLI users have to start it. SDK detected the refused
  connection and printed `xinsp-backend.exe &` instructions, which
  was excellent.
- What I tried: ran the driver cold, got a clear error.
- What worked: started backend manually with `VSLANG=1033` (per
  skill notes for clean cl.exe output).
- Time lost: ~1 minute. This is "annoying but well-handled" rather
  than friction.

### F-6: cl.exe diagnostics in mojibake on Chinese locale
- Severity: **P2**
- Root cause: cl.exe emits CP-950 messages on this Windows install;
  backend forwards them as bytes; SDK can't decode. The skill notes
  the workaround (`VSLANG=1033`) which fixed it for the backend log,
  but the build_log returned in `ProtocolError.data["diagnostics"]`
  was already English (cl.exe used the env from when it was
  compiled). So in the *ProtocolError* message I got readable
  English; in the *backend stdout log* I got mojibake. Not blocking
  but jarring.
- What I tried: started backend with `VSLANG=1033`. Confirmed
  backend log readable thereafter for *new* compiles.
- Time lost: ~0 minutes (skill warned me).

## What was smooth

- **Source plugin pattern was a clean port from `synced_cam`.** Worker
  thread + `image_create` + `emit_trigger` + sleep_until — once you
  see one source plugin, you can write the next in 10 minutes.
- **`xi::current_trigger() + .image("source0")` "just worked"** as
  documented; `is_active()` guard handled the timer-fallback ticks
  cleanly. (~64 inactive ticks per 2 s = the 16 ms timer firing in
  parallel; that's expected per the docs.)
- **`open_project` re-applies parallelism live.** I was uncertain
  whether the parallelism block could be re-read between sweeps
  without backend restart — it can. Each sweep saw the new
  `dispatch_threads` value reflected in `dispatch_stats`. Backend
  log even prints `continuous mode: 16ms timer + N dispatcher
  thread(s) + trigger bus` per `cmd:start` — useful.
- **`dispatch_stats` `dropped_oldest` counter is exactly what you
  need to see drop policy working.** Sweep 1's 103 drops was the
  smoking gun for "you're producing faster than you can consume".
- **Speedup measurement was painless.** One Python diff:
  `r2.throughput / r1.throughput`. The infrastructure to run three
  isolated sweeps in one driver took ~30 lines.
- **`compile_and_load` enriched ProtocolError** included full cl.exe
  diagnostics inline — found the int64_t ambiguity instantly without
  having to dig into a log file.
- **Backend resilience.** Three back-to-back `open_project` cycles
  with two project plugins each (so 6 cl.exe invocations) and one
  hot script reload, all in ~30 s, no crashes, no leaks observable.
