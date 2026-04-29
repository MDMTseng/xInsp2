# FL r7 — fuzz survey of 4 attack surfaces

A fuzz survey of four xInsp2 backend attack surfaces. Goal: catch
crashes / UB / DOS in the new boundary parsers (PR #25 manifest
validation, PR #26 IPC reader thread) before they ship.

This is a **survey, not a fix**. Bugs found here are documented in
`FRICTION_FUZZ.md` for follow-up tickets.

## TL;DR

| # | Surface                  | iters | findings | severity   |
|---|--------------------------|-------|----------|------------|
| 1 | WS cmd JSON parser       | 1500+ | 1 (P1)   | DOS-ish    |
| 2 | project / instance / manifest validation | 500 | 0        | clean      |
| 3 | host-side IPC frame parser | 17  | **16 (15 P0 + 1 control)** | **CRITICAL** |
| 4 | emit_trigger / RPC frames (in-proc path) | 800 | 0 | clean    |

The headline finding is target #3: **the host backend crashes with
`0xC0000005` (ACCESS_VIOLATION) when a worker process disconnects in
almost any abnormal state**, including before sending any frames at
all (control case proved this). The crash takes down the entire
backend along with every other isolated worker it owns. This is the
exact scenario `isolation:process` exists to prevent.

## How to run

```
# smoke
FUZZ_ITERS=100 python examples/fl_r7_fuzz/run_all.py

# full
python examples/fl_r7_fuzz/run_all.py

# target #3 (needs evil_worker.exe built)
cmake --build backend/build --config Release --target evil_worker
python examples/fl_r7_fuzz/harness_evil_worker_host.py
```

## Per-target notes

### #1 WS cmd JSON parser (`service_main.cpp::handle_command`)

1500+ iters of ~24 strategies (deep nesting up to 2000, 1MB strings,
NaN/Inf, control-char names, args type mismatches, raw garbage,
binary-opcode injection, etc.). **No backend crash.** All malformed
inputs were either silently dropped or produced an error log.

One repeatable behavioural finding (P1): every ~50–200 iters the
backend's WS `accept` would refuse new connections briefly with
WSAECONNREFUSED — but `proc.poll()` showed the process alive and the
*next* connection attempt 1–2s later succeeded. See
`FRICTION_FUZZ.md` for the working theory (likely a single-thread WS
accept loop that's serving a slow parse, not a crash).

Result file: `_results_ws_cmd.json`. Recorded sample of last-5
payloads at the moment of stall: ordinary `set_param` with wrong-typed
args plus a control-char name. Nothing exotic.

### #2 project / instance / manifest validation (`xi_plugin_manager.hpp`, PR #25)

500 iters across 30+ strategies (BOMs, truncated JSON, duplicate
keys, manifest type mismatches, min>max, huge param arrays, unknown
isolation values, missing required keys, unicode keys, control
chars, `null` for required fields, etc.). **0 findings.**

Pingable after every iter. open_project either succeeded with
warnings or returned a clean ProtocolError. The validation layer is
robust; this is the boring outcome we wanted.

Result file: `_results_config.json`.

### #3 host-side IPC frame parser (`xi_ipc.hpp` + `xi_process_instance.hpp`, PR #26)

**P0 — backend crash on worker EOF.**

Mechanism: a `xinsp-worker.exe` exits abnormally (or simply
disconnects without speaking the protocol). The host's reader
thread, started by `ProcessInstanceAdapter::start_reader_()`, calls
`recv_frame()` which throws `runtime_error("pipe read EOF")`. That
exception is **not caught at the thread boundary**, so the thread
terminates via std::terminate, which calls abort(), which the SEH
filter records as ACCESS_VIOLATION at VCRUNTIME140.dll+0x12645
(crash dumps confirm: `xinsp-backend-*-20260430-*.json`).

Result of running 16 IPC fuzz strategies (bad magic, huge len,
opcode 0xFFFF, partial header, len mismatch, RPC_EMIT_TRIGGER with
huge image_count, RPC_CREATE with name_len overflow, etc.): **15 of
16 strategies crash the host.** The one survivor (strategy 10,
`many_zero_frames`) actually does succeed in sending 100 valid empty
frames and then exits cleanly — that's a clue: when frames arrive
*at all* before the EOF, some other thread's exception swallowing
catches the trailing EOF.

Control test (`_control_evil.py`): an evil_worker that simply
connects and immediately closes the pipe — sends ZERO frames —
**also crashes the host with the same ACCESS_VIOLATION**. So the
bug is purely in the EOF path of the reader thread, not in any
specific malformed payload.

Result file: `_results_evil_worker.json`. Crash-dump JSON:
`%TEMP%\xinsp2\crashdumps\xinsp-backend-*.json`.

Suspected fix area: wrap the recv_frame loop in
`ProcessInstanceAdapter::run_reader_()` (around xi_process_instance.hpp
line ~287) in a top-level `try { … } catch (…)` that flips an
`adapter_dead_` flag and exits the thread cleanly. The host can then
respawn the worker (the path already exists for legitimate worker
crashes — see line ~560 in the same file). The reader's exception
shouldn't escape its thread.

### #4 emit_trigger / RPC frames (in-proc path)

800 iters of `exchange_instance` calls with malformed args (huge
counts, negative counts, wrong types, missing fields, embedded NULs,
unicode keys, unknown commands, oversized strings) plus
`set_param` calls with similar perturbations. **0 findings.**

Backend stayed pingable across the full run. The trigger bus and
exchange-instance path are robust to this kind of abuse on the
in-process side. The out-of-process side (worker → host
RPC_EMIT_TRIGGER) is covered by target #3 (strategies 11–13) and
inherits the same P0 EOF crash because the host can't receive any
frames at all without reaching the EOF-crash path.

Result file: `_results_emit_trigger.json`.

## Files

- `harness_ws_cmd.py` — target #1
- `harness_config.py` — target #2
- `harness_evil_worker_host.py` + `evil_worker.cpp` — target #3
- `harness_emit_trigger.py` — target #4
- `_common.py` — shared backend-spawn / WS helpers
- `_minimize_ws_crash.py`, `_control_evil.py` — minimization helpers
- `run_all.py` — runs harnesses 1, 2, 4 sequentially
- `_results_*.json` — per-harness machine-readable findings
- `RESULTS.md` (this) — human summary
- `FRICTION_FUZZ.md` — friction encountered while writing the harness;
  inputs to FL r8.

## Negative results worth noting

- target #1 found **no crash, no UB, no memory leaks** in the JSON
  parser despite ~1500 hostile inputs across 24 strategies — the
  hand-rolled `parse_cmd` parser in `xi_protocol.hpp` is pleasingly
  defensive given its size.
- target #2 found **no crash** in the new manifest-validation layer
  (PR #25), suggesting the type-coercion and warning paths are
  thorough.
- target #4 found **no crash** in the in-process trigger surface —
  the bus correctly handles unknown source names, oversized param
  payloads, and malformed exchange commands.

The single-process attack surface (#1, #2, #4) is solid. The
multi-process attack surface (#3) has a P0.
