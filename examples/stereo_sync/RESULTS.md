# stereo_sync — FL測試 round 3

## Outcome
- Cycles observed: 40 (correlation window) / 80 (soak)
- Matched (left.seq == right.seq): 40/40 (correlation), 80/80 (soak) — 100%
- Backend survived 5 s soak: yes
- VERDICT: **PASS** *(after working around two friction points — see F-1, F-2)*

Headline numbers from `driver.py`:
```
[correlation] events=79  active+paired=40  matched=40 (100.0%)  unique_tids=40
              sample left_seq:  [67, 68, 69, 70, 71] ... [104, 105, 106]
              sample right_seq: [67, 68, 69, 70, 71] ... [104, 105, 106]
[soak]        events=156 active+paired=80  matched=80 (100.0%)  unique_tids=80
```

40 paired triggers in 2.5 s ≈ 16 Hz pair rate (sources tick at 20 Hz each
but the script only sees the bus-driven dispatches; the timer-fallback
ticks come through as `active==False` events and are not counted).
**Every paired event had identical seq numbers in both frames.** No
half-triggers, no missed correlations once the sources had each emitted
once.

## What I observed about trigger correlation

The framework is doing exactly what `xi_trigger_bus.hpp` documents — and
**only** what it documents. The 128-bit `tid` is the SOLE correlation
key in `AllRequired` mode; the bus's `pending_` map keys events purely
by tid. My plugin generates a deterministic tid from the running seq
counter (`tid.hi = constant`, `tid.lo = seq + 1`), so frame N from
`cam_left` and frame N from `cam_right` independently land on the same
bucket. As soon as the bus has seen one emit per source for a given
tid, it dispatches the combined `TriggerEvent` to the script's worker.

What this does NOT mean:
- **Timestamps don't matter for correlation.** I left `ts=0` in
  `emit_trigger` so the host clock fills in; cam_left and cam_right
  emit at slightly different wall times for the same tid (jitter from
  two independent worker threads), but the bus pairs them anyway.
  `TriggerEvent.timestamp_us` is just whichever source landed first.
- **Order of emit doesn't matter.** Whether cam_left or cam_right
  emits first for a given tid, the second one closes the pending event
  and fires the sink. `leader_source` records "first to arrive".
- **There is a window.** AllRequired evicts stale partials older than
  `window_ms` (default 100 ms; I set 1000 ms). At 20 Hz with two
  near-synchronised threads, the gap between matching emits is
  microseconds — well within the window. If the window expires before
  the second source emits, the partial is silently dropped (its image
  released). I did NOT see this happen in the soak.
- **No half-trigger delivery under AllRequired.** When only one source
  has emitted, the bus does not call the sink — the partial just sits
  in `pending_`. The script's `xi::current_trigger().is_active()` is
  only true when the framework actually dispatched a complete event.
  My script defensively checks `left.empty() || right.empty()` and
  would `VAR(half_trigger, true)` if it ever happened. Across the 120
  paired dispatches across both runs, that branch never fired. Good.
- **`Any` policy is one-frame-fires-immediately.** I tested this
  briefly. Each emit dispatches by itself; only one camera's image is
  in the event. So my script saw `has_left XOR has_right` per dispatch
  and `matched` was undefined. Useful for single-source workflows,
  exactly wrong for stereo correlation.

What surprised me, in order of impact:

1. **Default isolation kills source plugins.** A plugin instance
   defaults to `"isolation": "process"`, but `xinsp-worker.exe`'s
   host_api is built from `xi::ImagePool::make_host_api()` and never
   calls `install_trigger_hook()`. So `host_->emit_trigger` is
   `nullptr` inside the worker process. My worker thread null-derefs
   on the first emit, the worker process dies, ProcessInstanceAdapter
   respawns it, the new worker null-derefs again, and so on until the
   3-respawns-in-60s rate-limiter gives up. From the script's POV the
   bus simply never sees any emit. **This is F-1, P0.**
2. **`set_trigger_policy` cmd silently drops `required` if there's a
   space after the colon.** The arg parser uses
   `args_json.find("\"required\":[")` (no space) but Python's default
   `json.dumps` emits `"required": [` (with space). Result: required
   list parsed as empty, `is_complete_locked()` returns false because
   it requires non-empty required, and AllRequired never fires.
   Worse, the cmd then triggers `save_project_locked()` which **writes
   `required:[]` back to disk**, so subsequent `open_project` calls
   inherit the broken state until you hand-edit project.json. **F-2,
   P1.**
3. **Non-bus-driven dispatches still come through as `vars` events.**
   When `start` is on, the worker thread fires inspect on a timer
   fallback even when no bus event is queued. Those dispatches show up
   as `active==False` with no images. The script must handle this —
   my driver counts them separately as `no_trigger_ticks` and excludes
   them from the matched-rate. Mildly annoying but documented in
   `service_main.cpp` line 1420.

## Friction log

### F-1: `xinsp-worker.exe` host_api has `emit_trigger == nullptr` — source plugins under default (process) isolation null-deref on first emit
- Severity: **P0 / Bug**
- Root cause: missing wiring. `backend/src/worker_main.cpp:135` builds
  `static xi_host_api host = xi::ImagePool::make_host_api();` but never
  calls `xi::install_trigger_hook(host)`. `make_host_api` explicitly
  sets `api.emit_trigger = nullptr;` (`xi_image_pool.hpp:333`). The
  in-proc path at `xi_plugin_manager.hpp:1195` does install the hook,
  which is why my plugin works under `"isolation": "in_process"` —
  but the docs (`adding-a-plugin.md`, `host_api.md`) and the case spec
  itself say plugins default to `process` and emit_trigger is the
  documented way to drive multi-camera workflows.
- Minimal repro: any source plugin (mine, or `synced_stereo` from the
  global plugins dir) instantiated under default isolation. Backend
  log shows the symptom exactly:
  ```
  [ProcessInstanceAdapter] 'cam_left' RPC failed: pipe write failed — attempting respawn
  [worker] CREATE ok
  [ProcessInstanceAdapter] 'cam_left' respawned — new worker pid=… (count=1)
  [ProcessInstanceAdapter] 'cam_left' RPC failed: pipe write failed — attempting respawn
  ...
  [ProcessInstanceAdapter] 'cam_left' respawn restore-def failed
  ```
  The `pipe write failed` is the worker process dying mid-RPC because
  its own background thread already null-derefed in the
  `host_->emit_trigger(...)` call inside `run_loop_`.
- What I tried: read `host_api.md`'s null-check note —
  > `emit_trigger` is null on hosts older than the trigger bus
  > addition; plugins should null-check.

  This note made me think process-isolation hosts simply omit the API
  for back-compat, and that I'd need to fall back. But the bus IS
  there; it's just that the hook isn't wired into the worker's
  host_api. So the "fix" of null-checking would only work if I
  *abandoned* the trigger bus inside the worker — which is the entire
  point of the case.
  Then I tried `"isolation": "in_process"` per instance and it worked
  immediately.
- What worked: `instances/cam_*/instance.json` → add
  `"isolation": "in_process"`. Both cameras share the backend's
  address space, the in-proc path's `install_trigger_hook` runs, emits
  reach the bus.
- Time lost: ~25 minutes (most of it staring at silently-failing
  worker respawns; the symptom is "everything looks fine but no
  dispatches arrive").
- Suggested fix (not patched per instructions): add
  `xi::install_trigger_hook(host);` after line 135 of
  `backend/src/worker_main.cpp` (and possibly factor the wiring out so
  `make_host_api` always returns a fully-armed table — currently the
  caller has to remember to install the hook separately, which is the
  exact thing the worker forgot to do).
  Cross-process emit then needs the worker's emit_trigger to ALSO RPC
  the call back to the backend's TriggerBus, not just call
  `TriggerBus::instance().emit()` (which would emit into the worker's
  local bus singleton, where nothing is listening). That's the
  bigger task and probably why this isn't done yet — but the bug
  symptom (silent null-deref + respawn loop) is what makes this P0.
  At minimum it should fail loudly, not respawn-loop forever.

### F-2: `cmd:set_trigger_policy` arg parser is whitespace-fragile and clobbers `required` to `[]` on disk
- Severity: **P1 / Bug**
- Root cause: `service_main.cpp:2268` searches for the literal
  substring `"required":[` (no space after colon). Python's
  `json.dumps` defaults emit `"required": [` (with space). The
  substring match fails, `required` parses as empty, and the cmd
  proceeds to call `set_trigger_policy(pol, {}, ...)`. Then
  `save_project_locked()` rewrites `project.json` with the now-empty
  required list, persisting the breakage. Same fragility on the
  in-file parser at `xi_plugin_manager.hpp:1030` — the project.json
  block parser has the same `"required":[` substring assumption, so
  agents/users who hand-write project.json with a space after `:` will
  also see this.
- Minimal repro:
  ```py
  c.call("set_trigger_policy", {
      "policy": "all_required",
      "required": ["cam_left", "cam_right"],
      "window_ms": 1000,
  })
  # → required is silently empty; project.json gets rewritten
  #   with required:[]
  ```
  The cmd's RSP looks fine (returns the project state). The backend
  log says nothing. The next `start` gives you 0 dispatches.
- What I tried: I called `set_trigger_policy` from my driver as a
  belt-and-suspenders backup to the project.json `trigger_policy`
  block. After it ran and I saw 0 dispatches in `AllRequired` mode,
  I checked `project.json` on disk and found
  `"required":[]` overwriting my `["cam_left","cam_right"]`. (The
  IDE's auto-save-detection notice in my session caught this — I
  doubt I'd have spotted it without a source-control diff.)
- What worked: (a) put the policy in `project.json`'s `trigger_policy`
  block where the SAME parser bug applies but you can hand-write
  `"required":["cam_left","cam_right"]` with no space; (b) DON'T call
  `cmd:set_trigger_policy` from the driver. Removed it.
- Time lost: ~15 minutes.
- Suggested fix (not patched): use `xi::Json::parse` (cJSON) in
  the cmd handler instead of substring search. The codebase already
  has a JSON parser; this handler is one of the few places that
  rolls its own. Same fix for `xi_plugin_manager.hpp:1030`'s
  project.json block parser.

### F-3: docs imply emit_trigger works under default isolation
- Severity: P2 / Doc
- Root cause: `docs/guides/adding-a-plugin.md` "How do I emit images
  (camera / source)?" says

  > Call `host->emit_trigger(name, tid, ts, images, count)` from a
  > worker thread.

  immediately above the "Crash isolation? **Plugin instances default
  to running in their own xinsp-worker.exe**" section. Read in order,
  these read as compatible — but they aren't (F-1). Either the
  underlying behaviour needs to change so they ARE compatible, or the
  doc needs a "until process-isolation emit_trigger lands, source
  plugins must opt out via in_process" caveat.
- What I tried: built and tested expecting the docs to be accurate.
- What worked: in_process isolation per instance.
- Time lost: rolled into F-1's 25 min.

### F-4: the `synced_stereo` reference plugin would have hit the same bug
- Severity: P2 / Bug-cluster
- Root cause: `plugins/synced_stereo/synced_stereo.cpp` is the
  reference implementation pointed at by the case spec and by docs.
  It's plain C ABI, ships in `plugins/`, defaults to process
  isolation, and `host_->emit_trigger` from inside its `run_loop_` is
  the single thing it does that matters. So under the current backend
  it would null-deref the same way as my plugin — meaning the
  intended reference probably hasn't been exercised under default
  isolation since the isolation default flipped from in-proc.
  `vscode-extension/test/runMulticam.mjs` spawns its own backend with
  custom args and a randomly-picked port; it's plausible that path
  doesn't enable process isolation, so the e2e test passes despite
  the bug.
- What I tried: looked at synced_stereo as a model. Decided to write
  my own plugin rather than reuse since the case requires TWO source
  instances (synced_stereo emits both images from one source).
- What worked: my plugin under in_process did the job.
- Time lost: ~5 minutes (just confirming this).

## What was smooth

- **`xi::current_trigger()` and `t.image("cam_left")` ergonomics.**
  Once the bus actually dispatched events, the script side is
  beautiful. Three lines to read the pair, three lines of `VAR` to
  ship metrics back. Zero plumbing.
- **Project-plugin compile + hot-load.** The backend builds my
  `synced_cam.dll` from `plugins/synced_cam/src/plugin.cpp` on
  `open_project` with no ceremony. Diagnostics on compile-failure
  paths are useful and structured.
- **`xi::Plugin` base class.** `host()`, `name()`, ctor signature,
  the `XI_PLUGIN_IMPL` macro — all of it just worked. No boilerplate
  I had to debug.
- **Single-image-per-source key collapsing.** The bus uses just the
  source name (instance name) as the event key when `image_count==1`.
  So my script reads `t.image("cam_left")` directly, no
  `cam_left/img` slash convention to remember. Nice ergonomic.
- **Deterministic tid worked first try.** I was a little nervous about
  whether `tid.hi=const, tid.lo=seq+1` would actually correlate
  cleanly across two independent worker threads. Once F-1 + F-2 were
  cleared, the very first run was 40/40 matched. The `TidHash` /
  `TidEq` in the bus is well-behaved and the AllRequired window is
  generous enough that 50 ms scheduler jitter doesn't matter.
- **Python SDK error surface for compile errors.** I never saw a
  compile failure here, but the `_enrich_compile_error` shape (line +
  col + message + a fallback hint to `%TEMP%\xinsp2\script_build`)
  reads well. Good UX for a layer most agents would otherwise stub
  past.
- **`exchange_instance` for live introspection.** Being able to poll
  `cam_left.get_status` while debugging was how I figured out F-1
  (the first call returned a sensible-looking `{running:true,
  emitted:0}` and the second returned `{}`, which is what told me the
  worker had crashed mid-loop).
