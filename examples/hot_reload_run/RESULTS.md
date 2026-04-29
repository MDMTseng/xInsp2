# hot_reload_run — FL測試 round 4

## Outcome
- Total vars events: 64
- Pre-reload events: 31
- Post-reload events: 32
- xi::state() count survived reload: yes  (last_pre=31, first_post=33)
- xi::Param<int> threshold survived: **no**  (set to 137 pre-reload, observed as 100 — the v2 file-scope default — first frame post-reload)
- v2 version VAR appeared within 5 events of reload: yes  (appeared at event idx +0 after compile_and_load returned)
- Largest gap in vars stream: ~4000 ms  (around reload boundary — confirmed straddling t_pre_reload..t_reload_returned; the reload itself took 3937 ms wall-clock and the run was *fully stopped* across that window — see F-2)
- Backend ping() after stop: ok
- VERDICT: **FAIL**

## What I observed about hot-reload semantics

The reload boundary is *not* a graceful continuation. It's a stop / unload / compile / load / reset sequence. Concretely:

1. The instant `compile_and_load` is received, `service_main.cpp:1086` flips `g_continuous = false`, joins the worker thread, and tears down continuous mode entirely. The vars stream **stops**. There is no overlap, no last-tick-of-old-DLL-then-first-tick-of-new-DLL hand-off.
2. Compile takes ~4 seconds wall-clock (cl.exe cold) — during this entire window, *zero* vars events are emitted. That is a 4000 ms gap, not "a small documented gap."
3. After load_script() succeeds, `g_persistent_state_json` is restored into the new DLL via `g_script.set_state(...)` — `xi::state()` is preserved as documented. The count went from 31 (last pre-reload) up to 32 inside the reload window (one solitary event), then 33 immediately when the new DLL started ticking (we re-issued cmd:start). Continuity of state: confirmed.
4. **`xi::Param<T>` is NOT restored across compile_and_load.** The new DLL came up with `threshold=100` (the v2 file-scope default), even though the live value at the moment of reload was `137` (set via `cmd:set_param`). The backend keeps no in-memory cache of "current Param values" to replay into the freshly loaded DLL — only `cmd:load_project` / `cmd:compare_variants` / explicit `cmd:set_param` invoke the script's `set_param` callback.
5. **Continuous mode (`cmd:start`) does not resume automatically.** After compile_and_load returns, the timer thread is dead. The driver had to issue `cmd:start fps=20` again. Without that, the post-reload phase would have collected zero vars events and the case would have failed even harder.

What the docs predicted vs what happened:
- `docs/guides/writing-a-script.md` lifecycle box says the reload sequence is `get_state → unload → load → set_state → restore params → ready`. Step "restore params" is documented but **does not happen** in `cmd:compile_and_load` — only in `cmd:load_project`. The text underneath ("`xi::Param<T>` values (replayed by `xi_script_set_param`)") is misleading: the replay only happens on project load, not on save/recompile.
- The same lifecycle box implies a continuous run would smoothly continue across the reload. In reality, continuous mode is unconditionally stopped and not auto-resumed.
- `docs/protocol.md`'s `compile_and_load` entry doesn't mention either of these side effects (continuous-mode stop, Param-loss). The state-persistence story (with its `state_dropped` event and `XI_STATE_SCHEMA` macro) is documented in detail; the Param story isn't.

## Friction log

### F-1: xi::Param<T> values lost across compile_and_load
- Severity: **P0 / Bug**
- Root cause: missing feature (Param replay during compile_and_load)
- What I tried: Set `threshold=137` via `c.set_param("threshold", 137)` while v1 was running, verified it landed (last_pre_threshold=137 in vars), then triggered `compile_and_load(v2)`. First post-reload vars message had `threshold=100`.
- What worked: Workaround — the driver could re-issue the set_param after every compile_and_load. But that requires the client to track *every* Param it has ever touched, and there is no protocol signal that a reload happened in band with the cmd response (you'd have to react to the rsp).
- Repro:
    1. Compile any script with `xi::Param<int> threshold{"threshold", 100, ...}`.
    2. `c.set_param("threshold", 137)`.
    3. `c.compile_and_load(<same script>)` (or any new script with the same Param).
    4. Run once → `threshold` reads 100, not 137.
- Backend evidence: `service_main.cpp:1139..1232` saves+restores `xi::state()` JSON across the reload but does not save+restore `xi::Param` values. The only paths that call `g_script.set_param` are `cmd:set_param` (1511), `cmd:load_project` (1738), and `cmd:compare_variants` (910). `compile_and_load` is conspicuously absent.
- Time lost: ~10 minutes (suspected from the symptom, confirmed by reading service_main.cpp).
- Suggested fix: between save_state and load_script, also snapshot the param map (the script already exposes `get_state` for state JSON; a parallel `get_params` ABI or a backend-side echo of every set_param value would do it). Then replay in the same block where set_state is replayed.

### F-2: continuous mode silently stopped by compile_and_load
- Severity: P1
- Root cause: docs gap + arguably API design issue
- What I tried: Issued `c.call("start", {"fps": 20})`, then while it was running, `c.compile_and_load(...)`, expecting the stream of `vars` events to continue (per the writing-a-script.md lifecycle diagram which implies continuity).
- What I observed: 4-second silence in the vars stream during the reload, and the stream never resumed. Driver had to call `c.call("start", ...)` again post-reload.
- What worked: explicit re-arm. Driver detects "did I have to restart?" by attempting `start` and checking whether it was rejected with "already running" — works but is an unnatural API.
- Backend evidence: `service_main.cpp:1086` — unconditional `g_continuous = false`; the worker is joined; nothing rearms it on success.
- Time lost: ~5 minutes (immediately obvious from "had_to_restart=True" output, then confirmed in source).
- Suggested fix (one of):
    (a) document this clearly in `docs/protocol.md` under `compile_and_load`, and in the writing-a-script lifecycle diagram. Edit the diagram to add a "stop/restart continuous" arrow.
    (b) make compile_and_load remember it was running and auto-resume after load_script succeeds. The 4-second gap will still be there — that's the cost of cl.exe — but at least the run continues by itself.

### F-3: VAR macro shadowing produces a confusing redeclaration error
- Severity: P2
- Root cause: docs gap (the gotcha is documented, but the error you actually see is a `C2374 'count': redefinition`, not the VAR-macro angle)
- What I tried: First version of `inspect_v1.cpp` had `int count = ...; VAR(count, count);` — the second `count` arg conflicted with the macro-injected local of the same name.
- What worked: Renaming the local to `new_count` and writing `VAR(count, new_count);`.
- Time lost: ~3 minutes — the writing-a-script.md gotcha box flagged this exact pattern, which I'd read minutes earlier, so recovery was fast. Without that callout it would've been longer.
- Suggested fix: nothing urgent. The docs callout works. Maybe the cl.exe diagnostic could be intercepted and rephrased ("hint: VAR(name, expr) declares `name` as a local — was `name` already declared above?") but that's gold-plating.

## What was smooth

- The Python SDK's threading model: `_inbox_vars` is just a Queue; spawning a daemon thread to drain it while the main flow drives `start`/`compile_and_load`/`stop` was zero friction.
- `xi::state()` survived the reload exactly as documented. The count went 30 → 31 → 32 → 33 across the boundary; no off-by-one, no schema confusion (no `XI_STATE_SCHEMA` declared on either side, so the schema-mismatch path didn't fire — as expected).
- `compile_and_load`'s diagnostic enrichment was excellent — the `C2374 redefinition` error came back with file/line/severity already parsed (`_enrich_compile_error` in client.py), no need to dig through cl.exe stderr.
- The `version` VAR appeared in the very first post-reload event (offset +0), so the reload was deterministic enough — once the new DLL is loaded, every subsequent inspect call uses it. No half-loaded states observed.
- `c.ping()` after `c.call("stop")` worked first try; the backend recovered cleanly from the whole stop/reload/restart/stop sequence.
