# FL r8 — concurrency fuzz survey

Five harnesses targeting concurrency surfaces not covered by single-
client cmd fuzz (r7). Each harness auto-spawns its own backend.

| Harness                | rc | findings | fatals | What it exercises                                |
|------------------------|----|----------|--------|--------------------------------------------------|
| `emit_x_cmd`           | 0  | 1*       | 0      | trigger emit storm vs WS cmd hammer              |
| `open_close_cycle`     | 1  | 2        | **1**  | rapid open/close + worker-process cleanup        |
| `cmd_during_compile`   | 0  | 8*       | 0      | cmds while compile_and_load on WS handler thread |
| `set_param_storm`      | 0  | 2†       | 0      | set_param @ ~10 kHz during continuous mode       |
| `backend_kill`         | 0  | 4*       | 0      | taskkill backend mid-RPC; worker cleanup latency |

\* findings = per-iter stats summary, not bugs
† 1 stats summary + 1 false-positive (see FRICTION_FUZZ.md)

## How to run

```
python examples/fl_r8_concurrency/run_all.py
```

Or individually:

```
FUZZ_DURATION=12 python examples/fl_r8_concurrency/harness_emit_x_cmd.py
FUZZ_ITERS=30   python examples/fl_r8_concurrency/harness_open_close_cycle.py
FUZZ_ITERS=8    python examples/fl_r8_concurrency/harness_cmd_during_compile.py
FUZZ_DURATION=8 python examples/fl_r8_concurrency/harness_set_param_storm.py
FUZZ_ITERS=4    python examples/fl_r8_concurrency/harness_backend_kill.py
```

## Headline numbers (this session, 2026-05-09)

- **emit_x_cmd**: 1747 emits + 3480 cmds in 12 s, 0 errors, max emit latency 19.5 ms, max cmd latency 19.0 ms — concurrent emit + cmd path is healthy
- **cmd_during_compile**: 8/8 iters PASS. Cmds sent during the 3.7 s cl.exe window all received responses (just deferred until compile finished). No deadlocks, no out-of-order responses
- **backend_kill**: 4/4 iters PASS. Worker processes detected host death and exited within **~110 ms** of `taskkill /F` on the backend
- **open_close_cycle**: **1 fatal at iter 0** — see FRICTION_FUZZ.md P1
- **set_param_storm**: backend handled 82k+ rejection rsps in 8 s with no crash, but the harness's set_param values were always rejected (harness bug, see FRICTION_FUZZ.md F1)

## What this round confirmed is healthy

1. **Reader thread under emit + cmd contention** — the always-on reader from PR #26 is reliable under sustained mixed traffic; no dropped frames, no torn reads.
2. **WS-thread compile blocking** — `compile_and_load` blocks the WS handler for ~3.7 s but cmds queued behind it complete cleanly when it returns. No driver-side deadlocks.
3. **Worker death detection** — `xinsp-worker.exe` cleanly shuts down within ~100 ms when the backend host dies. No orphans observed across 4 kills.

## What this round found

See `FRICTION_FUZZ.md` for ranked findings.
