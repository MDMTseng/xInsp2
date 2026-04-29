# crash_recovery3 — FL測試 r2 regression #2

## Outcome
- Total frames: 10
- Crashing frames: 4
- Happy frames: 6
- Happy frames returning correct count: 6 / 6
- Backend survived: yes
- Post-recovery happy frame: passed (frame_01.png, threshold raised to 1000 via `exchange_instance`, returned the correct count of 12)
- VERDICT: **PASS**

Summary table from the driver:

```
frame           truth   obs  crash  ping_ok  err
--------------------------------------------------------------------------------
frame_00.png        5     5  False     True
frame_01.png       12    -1   True     True  plugin crashed: ACCESS_VIOLATION
frame_02.png        5     5  False     True
frame_03.png        5     5  False     True
frame_04.png       12    -1   True     True  plugin crashed: ACCESS_VIOLATION
frame_05.png       12    -1   True     True  plugin crashed: ACCESS_VIOLATION
frame_06.png        5     5  False     True
frame_07.png       12    -1   True     True  plugin crashed: ACCESS_VIOLATION
frame_08.png        5     5  False     True
frame_09.png        5     5  False     True
```

Two crashes back-to-back (frame_04 / frame_05) and one immediately after a
crash (frame_06: happy → correct count) — confirms the worker process keeps
running across multiple SEH absorptions, not just one.

## What I observed about crash handling

By default every instance lives in its own `xinsp-worker.exe`. That worker
wraps each `process()` call in `_set_se_translator`, so a write through a
null pointer (which is what my plugin does on overflow) becomes a per-call
error rather than a process death:

- The `xi::Record` that comes back across the IPC has `error` set to the
  string `"plugin crashed: ACCESS_VIOLATION"`. No fields beyond `error`,
  so `out["count"].as_int(-1)` quietly returns `-1`.
- The script-side just observes a Record with `error` populated; nothing
  cascades into the script's exception path. I surfaced it via
  `VAR(error, out["error"].as_string(""))` and `VAR(crashed, !error.empty())`.
- The very next `c.run()` (whether it'll trigger another crash or not)
  goes straight through — no IPC re-handshake delay observed; consecutive
  crash frames returned in normal time.
- `c.ping()` between every frame stayed green.
- `c.list_instances()` after the loop showed `counter` still registered.
- `c.exchange_instance("counter", {"command": "set_threshold", "value":
  1000})` after four crashes worked, and the next run on a previously-crashing
  frame returned the correct count (12). So the worker's plugin state isn't
  reset by an absorbed crash — `crash_when_count_above_` survived intact and
  was then mutated as expected.

Net: layer 1 (in-worker SEH catch) absorbed everything — I never saw the
process-respawn path fire in the backend log. Behaviour matches
`docs/reference/instance-model.md` "isolation modes" exactly.

## Friction log

### F-1: VAR(name, expr) gotcha — name shadowing across two VARs
- Severity: P2 (annoying but not blocking)
- Root cause: API design / docs covered, but the failure mode is verbose.
- What I tried: `auto err = out["error"].as_string(""); bool crashed =
  !err.empty(); VAR(crashed, crashed); VAR(error, err); VAR(count, ...);`.
  This faceplants because `VAR(crashed, crashed)` expands to `auto crashed
  = crashed;` shadowing my own local. cl.exe diagnostic was a wall of
  CJK-encoded C2374/C2086/C2371 (mojibake in this terminal) — readable
  but not pleasant.
- What worked: re-read `writing-a-script.md` "Gotcha — `VAR(name, ...)`
  declares a local". Inlined the expressions into the macro:
  ```cpp
  VAR(error,   out["error"].as_string(""));
  VAR(crashed, !error.empty());
  ```
  Note `error` from the first VAR is then in scope for the second's RHS —
  that side of the gotcha is actually convenient once you see it.
- Time lost: ~2 minutes.

The doc warns about this explicitly; the friction is just that the
compiler diagnostics don't point you at the macro. A note in the cl.exe
diagnostic ("did you VAR a name twice?") would shave the 2 min, but
that's a quality-of-life nit not a real defect.

### F-2: cl.exe diagnostics rendered as mojibake in the SDK error
- Severity: P2
- Root cause: the diagnostic strings cl.exe produces under the Big-5 /
  CP950 system locale come back as `?????w?q` etc through
  `ProtocolError.data["diagnostics"]`. The codes (C2374 / C2086 / C2371)
  + filename + line are intact, which is enough to localise — but the
  message text is unusable in this terminal.
- What I tried: read line numbers + codes from the diagnostic, jumped
  straight to `inspect.cpp:46`, identified the issue from the line text.
- What worked: same — line-and-code triangulation worked fine.
- Time lost: ~0 minutes (codes alone were enough this time).

Not a blocker for me, but agents that don't know the C2374 family by
heart would lose more time. Calling cl.exe with `/utf-8` for diagnostic
output would solve it without changing the SDK.

## What was smooth

- The required-reading list was right-sized. After
  `instance-model.md` + `plugin-abi.md` + `adding-a-plugin.md` +
  `writing-a-script.md` + `client.py` docstrings, I had everything I
  needed to write the plugin and driver without trial and error.
- `open_project` + `compile_and_load` Just Worked. The plugin was scanned,
  built, and instantiated on first try; instance.json's seeded
  `crash_when_count_above: 8` came through to `set_def` correctly (frames
  with 12 blobs crashed, frames with 5 didn't, exactly as configured).
- The worker's SEH translation does what the docs say it does. No
  surprises. The error string `"plugin crashed: ACCESS_VIOLATION"` is
  pleasantly diagnostic — clearly distinguishable from a plugin-thrown
  C++ exception or a misconfigured input.
- `exchange_instance` after four absorbed crashes worked first-try and
  the worker's in-memory state (the threshold field) was intact. The
  "respawn replays last set_def" promise wasn't even needed here — layer
  1 alone preserved state.
- `c.ping()` returns instantly even right after a crash; the IPC
  pipe-and-worker isn't lagging recovery.
- Total time from "PLAN.md done" to "VERDICT: PASS": about 8 minutes,
  including the F-1 fix and one round trip to start the backend.

## Files

- `plugins/count_or_crash/plugin.json` + `src/plugin.cpp` — the plugin
- `instances/counter/instance.json` — instance with default threshold 8
- `project.json`, `inspect.cpp` — script
- `generate.py` — synthesises 10 PNGs (6 happy / 4 crashing)
- `ground_truth.json` — per-frame blob counts
- `driver.py` — the harness
- `driver_summary.json` — machine-readable run summary
- `backend.log` — backend stdout from this run
