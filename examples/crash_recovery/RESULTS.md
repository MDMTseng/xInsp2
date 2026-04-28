# crash_recovery — FL測試 round 2

## Outcome
- Total frames: 10
- Crashing frames: 3
- Happy frames: 7
- Happy frames returning correct count: 7 / 7
- Backend survived: yes
- Post-recovery happy frame (frame_02.png, truth=12): status=ok, count=12
- VERDICT: PASS
- elapsed: 0.1s

### Per-frame

| i | file | kind | truth | observed | crashed? | retry | ping ok |
|---|---|---|---|---|---|---|---|
| 0 | frame_00.png | happy | 5 | 5 | no |  | Y |
| 1 | frame_01.png | happy | 4 | 4 | no |  | Y |
| 2 | frame_02.png | crash | 12 | (error) | yes |  | Y |
| 3 | frame_03.png | happy | 5 | 5 | no |  | Y |
| 4 | frame_04.png | crash | 13 | (error) | yes |  | Y |
| 5 | frame_05.png | happy | 4 | 4 | no |  | Y |
| 6 | frame_06.png | happy | 5 | 5 | no |  | Y |
| 7 | frame_07.png | crash | 12 | (error) | yes |  | Y |
| 8 | frame_08.png | happy | 5 | 5 | no |  | Y |
| 9 | frame_09.png | happy | 4 | 4 | no |  | Y |

### Detail per frame
- [0] frame_00.png (happy, truth=5): status=ok, count=5, detail=`ms=0`
- [1] frame_01.png (happy, truth=4): status=ok, count=4, detail=`ms=0`
- [2] frame_02.png (crash, truth=12): status=error, count=None, detail=`no count VAR / count == -1`
- [3] frame_03.png (happy, truth=5): status=ok, count=5, detail=`ms=0`
- [4] frame_04.png (crash, truth=13): status=error, count=None, detail=`no count VAR / count == -1`
- [5] frame_05.png (happy, truth=4): status=ok, count=4, detail=`ms=0`
- [6] frame_06.png (happy, truth=5): status=ok, count=5, detail=`ms=0`
- [7] frame_07.png (crash, truth=12): status=error, count=None, detail=`no count VAR / count == -1`
- [8] frame_08.png (happy, truth=5): status=ok, count=5, detail=`ms=0`
- [9] frame_09.png (happy, truth=4): status=ok, count=4, detail=`ms=0`

### exchange_instance + post-recovery

```
exchange reply: {"crash_when_count_above": 999, "frames_processed": 10, "last_count": 4}
post_recover  : {"file": "frame_02.png", "truth": 12, "status": "ok", "count": 12, "detail": "ms=0"}
```

### Project shape
```json
{
  "proj_plugins": [
    "json_source",
    "blob_analysis",
    "data_output",
    "mock_camera",
    "record_save",
    "count_or_crash",
    "synced_stereo"
  ],
  "proj_instances": [
    [
      "cnt",
      "count_or_crash",
      "<unset>"
    ]
  ]
}
```

### list_instances after the loop
```json
[
  {
    "name": "cnt",
    "plugin": "count_or_crash"
  }
]
```

## What I observed about crash handling

The plugin's `process()` does a hard `*(volatile int*)nullptr = 42`. With the default (un-set) `isolation` field in `instance.json`, the backend ran the instance in a separate worker process — the backend log shows `[ProcessInstanceAdapter] 'cnt' spawned worker pid=... pipe=xinsp2-pipe-...` at open time. So the **default really is `"process"` isolation**, matching `docs/reference/instance-model.md` and contradicting the `docs/guides/adding-a-plugin.md` statement that crash isolation is via `_set_se_translator` (which describes the legacy in-proc behaviour, not what ships).

What I expected, after re-reading instance-model.md: each crash kills the worker → backend logs a respawn line → next call works because the adapter re-spawns and replays `set_def`.

What actually happened (backend stderr):

```
[worker] plugin SEH: 0xC0000005 (ACCESS_VIOLATION)
[xinsp2] use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION
[worker] plugin SEH: 0xC0000005 (ACCESS_VIOLATION)
[xinsp2] use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION
[worker] plugin SEH: 0xC0000005 (ACCESS_VIOLATION)
[xinsp2] use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION
```

Three crashes, **no respawn lines between them**, and the worker `pid` is logged exactly once — the worker process's SEH handler catches the AV inside the worker, replies "I crashed" to the backend over the pipe, and the worker itself stays alive ready for the next call. So in this build crash recovery is doing something subtler than what instance-model.md describes: instead of a per-crash worker respawn, the worker has its own SEH wrapper that converts an AV into an in-band error reply. The instance never gets torn down; no `set_def` replay is needed.

Surface to the Python SDK side: `c.run()` **succeeds** on a crashing frame (no `ProtocolError` raised). The script's `cnt.process()` call returns a record with **no** `count` field and **no** `error` field — just empty. That's why my driver's `try_run` ends up reporting `status=error, detail="no count VAR / count == -1"`: my inspect.cpp uses `out["count"].as_int(-1)` as a sentinel, and the only thing that flagged "the plugin crashed" was the absence of expected output, not anything explicit. From the script author's POV, an isolated crash looks identical to the plugin returning `xi::Record{}`. The backend log is the only place an explicit "plugin crashed: ACCESS_VIOLATION" message exists.

Side observation: even though the per-frame `c.ping()` succeeded after every crash, that proves the **backend** survived but doesn't prove the **instance** survived. The post-loop `c.list_instances()` confirmed `cnt` was still registered, and the post-loop `exchange_instance` reply showed `frames_processed=10` (every `process` call was counted, including the crashing ones — so the worker did get to `++frames_processed_` before `*nullptr = 42`), and the post-loop re-run with `crash_when_count_above=999` returned the correct count. Three independent confirmations the instance is fully live.

## Friction log

### F-1: Docs disagree on what "crash isolation" means
- Severity: P1 (had to work around)
- Root cause: docs gap
- `docs/guides/adding-a-plugin.md` (the on-ramp doc a new plugin author
  reads first): "Same-process plugins are protected by
  `_set_se_translator`: a segfault in `process()` becomes an exception
  and the backend stays up. For deeper isolation (separate process), see
  the `shm-process-isolation` spike on its branch — `instance.json` gains
  `"isolation": "process"` opt-in."
- `docs/reference/instance-model.md` (the reference doc): "**Default:
  process.** A new instance with no `isolation` field in its
  `instance.json` runs in its own `xinsp-worker.exe`."
- Reality (this run): default behaviour matches the reference doc, not
  the guide. The guide reads as if `shm-process-isolation` is still
  branch-only and opt-in; in fact the spawned worker, the SHM region, and
  the `ProcessInstanceAdapter` are all live on `main`.
- What I tried: read both docs, planned for either world, observed the
  backend log to settle it.
- What worked: trusting the reference doc + observing the backend log
  (`[ProcessInstanceAdapter] 'cnt' spawned worker pid=...`) to confirm
  what actually fired.
- Fix: `docs/guides/adding-a-plugin.md` "Crash isolation?" Q&A needs the
  same rewrite the reference got — point at `instance-model.md`, mention
  the worker-process default, drop the "see the spike branch" line.
- Time lost: ~5 minutes (mostly while writing PLAN.md, deciding which
  fallback to plan for).

### F-2: A crashed `process()` returns silently from the script's POV
- Severity: P1 (had to work around)
- Root cause: API design issue (or maybe just a docs gap — the design
  may be intentional, but a plugin/script author can't tell)
- When the worker SEHs, the script-side `xi::use("cnt").process(input)`
  returns a `Record` with no fields at all. There's no `error` key, no
  `crashed` key, no exception, nothing. The only signal is "the outputs
  you expected aren't there." The backend logs
  `use_process('cnt') isolated: plugin crashed: ACCESS_VIOLATION` but
  the script never sees that string.
- What I tried: had the script set a sentinel `count=-1` if `out["count"]`
  is missing, AND check for an `out["error"]` key in case the framework
  injected one. Neither caught the crash explicitly — the
  `count==-1` sentinel did.
- What worked: relying on the absence of expected output + the per-frame
  `c.ping()` for backend liveness. Adequate for this test, but a
  production script that wants to differentiate "plugin disagreed with
  the input" from "plugin crashed" can't, without scraping the backend
  log.
- Suggested fix: when `ProcessInstanceAdapter` swallows a crash, inject
  a synthesised `error` key (e.g. `{"error": "plugin crashed:
  ACCESS_VIOLATION"}`) into the Record returned to the script. The
  current empty-Record return is observationally indistinguishable from
  a plugin choosing to return nothing.
- Time lost: ~10 minutes (driver had to be slightly more defensive and
  I had to read backend stderr to be sure what happened).

### F-3: instance-model.md describes "auto-respawn" but the live
        behaviour is "in-process SEH catch + same worker continues"
- Severity: P2 (annoying but not blocking)
- Root cause: docs gap
- instance-model.md: "A buggy plugin can crash its worker process
  without taking the backend with it; `ProcessInstanceAdapter`
  auto-respawns the worker (rate-limited 3/60 s) and replays the last
  `set_def` so the next call still works."
- Reality: the worker process didn't die at all — its `pid` was logged
  once at open, and three subsequent ACCESS_VIOLATIONs are logged with
  no respawn line in between. The worker has its own SEH wrapper that
  converts the AV into a "plugin crashed" reply over the pipe; the
  worker itself keeps running.
- This is arguably *better* than what the docs describe (no IPC
  re-handshake / DLL re-load between crashes), but it means the
  "rate-limited 3/60 s" sentence is misleading — that limit only
  matters if the worker actually dies (e.g. a process-wide std::abort
  the SEH wrapper can't catch, or the worker getting OOM-killed).
- Suggested fix: clarify that there are two layers — (a) the worker's
  SEH wrapper catches AVs inside `process()` so the pipe stays up and
  the same worker handles the next call; (b) only if the worker
  process itself terminates does the adapter re-spawn (rate-limited).
- Time lost: ~5 minutes reconciling logs vs docs.

## What was smooth

- `open_project` compiled the project-local `count_or_crash` plugin
  without any extra build wiring. Drop a `plugin.json` + `src/plugin.cpp`
  in `plugins/<name>/`, list nothing in `project.json` instance config,
  ship an `instances/<name>/instance.json`, and the backend handles the
  rest.
- The `xi::Plugin` base + `XI_PLUGIN_IMPL` macro made the plugin source
  trivial — about 80 lines total including the deliberate-crash branch.
- Backend startup: SDK error message (`ConnectionRefusedError` with a
  hint about `xinsp-backend.exe &`) was clear, started cleanly.
- `c.ping()` after every frame is a cheap, reliable backend-liveness
  check. It wasn't enough on its own to prove instance-liveness, but
  combined with `list_instances` + a follow-up `process` call it
  triangulates the answer with no need to inspect backend internals.
