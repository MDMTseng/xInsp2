# FE/BE split — frontend supervisor + safe-state seam

## Why

Process isolation + the SHM mesh were removed in 2026-05 (see
[`reference/ipc-shm.md`](../reference/ipc-shm.md), now historical). Every plugin —
cameras included — runs **in-process** inside the backend (BE) compute core
(`xinsp-backend.exe`). The trade was deliberate: *a dead plugin means a dead
pipeline regardless of isolation, so per-plugin sandboxing only bought
complexity.* But a deployed line still needs something to **keep the machine
safe and the backend running** when the BE dies.

That is the **frontend supervisor (FE)**, `xinsp-fe.exe`. It owns the BE
process lifecycle and the line's safe state; the BE owns all compute.

## The boundary

```
  PLC / line  ◀── SafeStateSink ──┐ enter_safe_state(reason+forensics) on BE death
                                  │ clear_safe_state() on healthy resume
   xinsp-fe.exe  (supervisor)     │
     spawn → monitor → on death: safe-state + read crash report → respawn (rate-limited)
        │ CreateProcess + Job Object (kill-on-close)
        ▼
   xinsp-backend.exe  (compute core)
     --project/--script/--autostart-fps → self open_project → compile_and_load → start
     all plugins in-process; fatal crash → minidump + JSON crash report
```

- **BE = compute.** Cameras, detectors, ops, the inspection script — all
  in-process. On a fatal fault it writes a minidump + JSON crash report
  (`SetUnhandledExceptionFilter`, see [`../guides/debugging.md`](../guides/debugging.md))
  and exits.
- **FE = lifecycle + safety.** No image processing. It never loads a plugin. It
  spawns the BE, watches it, drives the line safe on death, and respawns.

## SafeStateSink — the PLC seam

`backend/include/xi/xi_safe_state.hpp` (portable C++; no Win32/OpenCV):

```cpp
enum class SafeStateReason { BackendExit, PortUnresponsive, RespawnLimitExceeded, SupervisorShutdown };
struct SafeStateEvent { reason; backend_rc; exception_name; faulting_module; last_phase; report_path; ts_ms; };
class SafeStateSink { void enter_safe_state(const SafeStateEvent&); void clear_safe_state(); const char* name(); };
```

`make_safe_state_sink(type)` picks an implementation. Today the only concrete
sink is **`LoggingSafeStateSink`** — a stub that records the transition to the FE
log. A real PLC transport (Modbus coil, OPC-UA node, digital-out) is **Phase 2**:
it implements `SafeStateSink` and slots into the factory; nothing else in the FE
changes. This keeps the "what commands the line" decision out of the supervisor
loop until the hardware is known.

## Backend headless autostart

So the FE needs no WebSocket client (there is no C++ WS client —
`xi_ws_server.hpp` is server-only), the BE self-runs a project from the command
line. After binding its WS port, `service_main` synthesizes the same wire
commands a client would send:

| Flag | Effect |
|---|---|
| `--project=DIR` | `open_project {"path":DIR}` at boot |
| `--script=PATH` | script to `compile_and_load` (default: the project.json `script`) |
| `--autostart-fps=N` | if N>0, `start {"fps":N}` continuous mode |

Reply frames are no-ops while no client is connected; the WS port stays open so
an operator HMI / the VS Code extension can still attach live.

## Supervisor behavior

- **Spawn** (`fe_main.cpp`, Win32): `CreateProcessA` with the BE + composed
  flags; child stdout/stderr → `be.log` (inherited handle) so the FE can read the
  crash output; child assigned to a **Job Object** with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` so the BE can never orphan if the FE dies.
- **Monitor**: `WaitForSingleObject` on the process + a shallow TCP `connect`
  probe of the WS port between waits. The probe drives `clear_safe_state` on a
  healthy resume; `PortUnresponsive` is flagged only after N consecutive probe
  failures while the process is alive. A **boot-readiness gate** withholds
  "healthy" until the BE log shows `autostart: ready` — the port binds before
  the synchronous open/compile, so port-up ≠ serving; a BE that hangs before
  ready within `--boot-timeout-ms` is driven `BootTimeout` → respawn. A BE whose
  script **fails to compile** logs `autostart: degraded` and withholds the
  `ready` marker, so the FE drives it safe rather than trusting port-up — a
  non-inspecting line is never reported healthy. (A serve-time wedge — port
  still accepting but commands stalled — needs a deep WS heartbeat and stays
  Phase 2.)
- **On death — forensics**: the FE parses `be.log` for the last
  `minidump: <path>.dmp` line the BE printed, reads the sibling `.json`, and
  pulls `exception.name`, `exception.module`, and the dispatch-thread
  `context.last_phase` / `threads[]` breadcrumb into the `SafeStateEvent`. (It
  parses the log rather than scanning `%TEMP%/xinsp2/crashdumps` because a
  sandboxed/per-tool `TEMP` isn't inherited by the spawned BE.)
- **Respawn**: sliding 60s window, cap 5, 1.5s backoff (mirrors the VS Code
  extension's existing supervisor). On exceeding the cap the FE drives
  `RespawnLimitExceeded` and **stays safe**, awaiting a manual restart — it does
  not spin.
- **Shutdown**: a console-ctrl handler closes the Job Object (killing the BE) and
  drives `SupervisorShutdown`.

## VS Code extension: managed vs attach

The extension is itself a supervisor in dev (it spawns + respawns the BE). To
avoid two supervisors fighting over one BE, it has a backend **mode**
(`xinsp2.backendMode`):

- **`managed`** — extension owns the BE (today's dev inner-loop).
- **`attach`** — a BE is already running (FE-owned on a line); the extension
  connects read/operator-only and never spawns or respawns.
- **`auto`** (default) — attach if the port is already open, else managed.

In attach mode the extension surfaces a safe-state status and reworded crash
messaging, and its respawn path is disabled. See
[`../guides/extending-the-ui.md`](../guides/extending-the-ui.md).

## Verified by

`examples/fe_supervisor/` — arms a plugin to crash the BE on the first inspect
under autostart, then asserts the FE drove safe-state with crash forensics,
respawned, hit the cap, stayed safe, and left no orphan.
`examples/plugin_crash_forensics/` covers the BE-side minidump/breadcrumb the FE
relies on.

The full test plan — unit / integration / e2e / safety-property coverage, what
exists today vs the priority gaps — is in
[`fe-be-split-test-plan.md`](./fe-be-split-test-plan.md).

## Phase 2 (not built)

- C++ WS client + deep heartbeat (detect a hang while the port stays open).
- An FE **status channel** (local endpoint / status file) so the extension shows
  the FE's *true* safe-state + respawn budget instead of inferring from a WS drop.
- Real PLC transports behind `SafeStateSink` (Modbus / OPC-UA / digital-out).
- Operator HMI; multi-BE / multi-line orchestration.
- Linux supervisor implementation (`posix_spawn`/`fork`, `PR_SET_PDEATHSIG`).
