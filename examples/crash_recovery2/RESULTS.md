# crash_recovery2 — FL測試 r2 regression

## Outcome
- Total frames: 10
- Crashing frames: 5
- Happy frames: 5
- Happy frames returning correct count: 5 / 5
- Backend survived: yes
- Instance still listed after crash storm: yes
- Post-recovery happy frame: observed=12, truth=12
- VERDICT: **PASS**

## Per-frame

| frame | truth | observed | crashed? | next_call_ok? |
|---|---|---|---|---|
| frame_00.png | 5 | 5 | False | True |
| frame_01.png | 12 | -1 | True | True |
| frame_02.png | 5 | 5 | False | True |
| frame_03.png | 5 | 5 | False | True |
| frame_04.png | 12 | -1 | True | True |
| frame_05.png | 12 | -1 | True | True |
| frame_06.png | 5 | 5 | False | True |
| frame_07.png | 5 | 5 | False | True |
| frame_08.png | 12 | -1 | True | True |
| frame_09.png | 12 | -1 | True | True |

## What I observed about crash handling

The framework absorbed 5 crashes inside the plugin's `process()` and the next call always succeeded (5/5 of them had a successful follow-up call). The script-side mechanism the docs predicted (a Record with `error` set on the crashed call) is exactly what we saw: `det.process()` returned a Record whose `error` string was non-empty, the inspect script noticed and emitted `crashed=1` + `count=-1`, and `c.run()` itself completed normally — it did NOT raise on the Python side.

Happy frames came through with the correct count (e.g. frame_00.png: truth=5, observed=5).

Documentation accuracy: `docs/reference/instance-model.md` "isolation modes" describes process isolation as the default and predicts the exact two-layer recovery (in-worker SEH catch + process respawn). Our case only exercises layer 1 — every crash was caught in-worker and replied as a per-call error; the worker process didn't need to be respawned. The docs were accurate. The one thing not explicitly written down is what `Plugin::process()` returning a Record with `error` actually looks like to the script — a quick example like the one in `docs/guides/adding-a-plugin.md` was enough to bridge that.

## Friction log

### F-1: `VAR(mask, mask)` collides with the local `auto mask` it expanded from
- Severity: P2 (annoying but not blocking)
- Root cause: API design / docs gap — the trap is mentioned in passing inside `examples/circle_counting/inspect.cpp` ("VAR expands to `auto NAME = expr;` so we name the local differently to avoid the redeclaration trap") but it's not in `docs/guides/writing-a-script.md` or anywhere a first-time author would look. cl.exe's diagnostic (`'mask': cannot be used before initialization`) doesn't point at the macro.
- What I tried: `auto mask = out.get_image("mask"); ... VAR(mask, mask);` → `error C3536`.
- What worked: rename the local to `mask_img`; `VAR(mask, mask_img)` compiles. Cost: one round-trip to the script_build log under `%TEMP%\xinsp2\script_build\inspect_v3.log` because the ProtocolError surfaced only as `compile failed` — see F-2.
- Time lost: ~3 minutes

### F-2: `compile_and_load` ProtocolError says only `compile failed`; the cl.exe diagnostic isn't on the wire
- Severity: P2 (annoying but not blocking)
- Root cause: unclear error message. The build log lives at `%TEMP%\xinsp2\script_build\inspect_v<N>.log` and contains the actual cl.exe error/warning lines, but neither the SDK's `ProtocolError` text nor the backend's stderr stream (`xinsp-backend.exe` redirected to `backend.log`) carry them. An agent / developer has to know that path exists. The Python SDK's `compile_and_load` docstring doesn't mention it; `docs/guides/writing-a-script.md` could call it out.
- What I tried: `c.on_log(...)` to capture log messages; got nothing useful. Tailed `backend.log`; nothing there either.
- What worked: `ls -lt %TEMP%/xinsp2/script_build/*.log | head` and reading the newest one. The fix-loop after that was instant.
- Time lost: ~5 minutes

## What was smooth

The crash-recovery story itself was textbook. The docs at `docs/reference/instance-model.md` "isolation modes" + `docs/guides/adding-a-plugin.md` "Crash isolation?" predicted the exact behaviour we saw: process isolation is the default with no `instance.json` opt-in needed, the worker SEH wrapper catches the AV, the script gets a Record with `error` set, the next call goes straight through, no respawn fired (worker pid stayed identical across the storm). `c.run(frame_path=...)` + `c.exchange_instance(...)` were enough to express the whole driver. Backend log even printed `[ProcessInstanceAdapter] 'det' spawned worker pid=<N>` once and never again — clean signal that layer 2 wasn't needed.

The plugin authoring path (subclass `xi::Plugin`, `pool_image()`, `cv::*` directly, `XI_PLUGIN_IMPL`, `plugin.json`) is concise enough that a new plugin took ~30 lines, and the in-place build (`plugins/<name>/build/plugin_v<N>.dll`) on `compile_and_load` worked first try with no explicit cmake step. No framework code had to be modified.

## Raw observations

- c.ping() after loop: ok ({'pong': True, 'ts': 1777421376.66})
- list_instances names=['det'] (det listed: True)
- exchange_instance(set_threshold,1000) -> {'crash_when_count_above': 1000, 'frames_processed': 10, 'last_count': 12}
- post-recovery run on frame_01.png: count=12 crashed=False truth=12

## Relevant backend logs

```
(no warn/error/crash log lines captured)
```