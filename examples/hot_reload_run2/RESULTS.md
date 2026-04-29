# hot_reload_run2 — FL測試 r4 regression

## Outcome
- Total vars events: 70
- Pre-reload events: 38
- Post-reload events: 32
- xi::state() count survived reload: yes  (last_pre=38, first_post=39)
- xi::Param<int> threshold survived: yes  (last_pre=137, first_post=137)
- v2 version VAR appeared within 5 events of reload: yes (offset=0)
- Largest gap in vars stream: 8156 ms (straddles reload boundary: yes)
- Run resumed automatically (no manual cmd:start): yes
  - compile_and_load rsp included `resumed_continuous: true`: yes
- Backend ping() after stop: ok
- VERDICT: PASS

## What I observed about hot-reload semantics
The framework handled the reload exactly as the brief described, and the behaviour is BARELY discoverable from the user-facing docs:
- `docs/guides/writing-a-script.md` documents that `xi::state()` and   `xi::Param<T>` survive reloads (good — both observed).
- `docs/protocol.md` describes `compile_and_load` rsp shape as   `{build_log, instances, params}` and does NOT mention either   `dll` or `resumed_continuous` keys, even though the backend   emits both. I only knew to check `resumed_continuous` because   the brief told me. A naive client following protocol.md would   never look for it.
- Neither `cmd:start` nor `cmd:stop` is documented in   `docs/protocol.md` at all (only mentioned in passing in   writing-a-script.md re. the trigger guard). The fps arg,   reply shape `{started:true}` / `{already:true}`, and the   fact that `compile_and_load` auto-tears-down + auto-rearms   the worker are all undocumented.
- The auto-resume + param-replay + state-restore path lives in   `service_main.cpp` around lines 1095–1340 and is comprehensive   (including schema-version drop on mismatch via XI_STATE_SCHEMA),   but a user would only know it exists by reading the backend.

## Friction log

<!-- run note: set_param threshold=137 at wall_ms~85631625 -->
<!-- run note: compile_and_load(v2) took 8.06s; rsp keys: ['diagnostics', 'dll', 'resumed_continuous']; resumed_continuous=True -->

### F-1: `cmd:start` and `cmd:stop` undocumented in protocol.md
- Severity: P1
- Root cause: docs gap
- What I tried: searched `docs/protocol.md` for `start`/`stop` — only   found `cmd:start fps=N` mentioned offhand in writing-a-script.md.
- What worked: read `backend/src/service_main.cpp` to see the   handler signatures (`fps` arg, default 10, `{started:true}` rsp).
- Time lost: ~3 minutes

### F-2: `resumed_continuous` rsp field undocumented
- Severity: P1
- Root cause: docs gap
- What I tried: protocol.md says `compile_and_load` returns   `{build_log, instances, params}`. Brief said look for   `resumed_continuous: true`.
- What worked: confirmed in `service_main.cpp` line 1337.
- Time lost: ~1 minute (would have been zero without the brief).

### F-3: SDK has no first-class continuous-mode vars iterator
- Severity: P2
- Root cause: missing feature
- What I tried: `Client.run()` is single-shot; no `iter_vars()` /   `subscribe_vars()`. Calling `c.run()` during `cmd:start` would   send a stray `cmd:run` and fight the worker.
- What worked: pull `c._inbox_vars.get(timeout=...)` directly.   Functional but uses a private attribute and a writer of   external code would feel uneasy about it.
- Time lost: ~5 minutes

## What was smooth
- `compile_and_load` returned promptly; cl.exe gap was the only   noticeable pause.
- `xi::state().set('count', n)` + `xi::state()['count'].as_int(0)`   worked as documented; no surprises.
- `xi::Param<int>` value really did survive the reload — `137`   carried straight across.
- The case ran end-to-end on the first try once the driver was   written.