# FE / BE split — supervisor, safe-state, crash history

**Shipped design-of-record.** Process isolation + the SHM mesh were removed
2026-05 — every plugin (cameras included) runs **in-process** in the backend (BE)
compute core. The trade was deliberate (*a dead plugin means a dead pipeline
regardless of isolation*), but a deployed line still needs something to keep the
machine safe and the backend running when the BE dies. That's the **frontend
supervisor (FE)**, `xinsp-fe.exe`: it owns the BE process lifecycle and the line's
safe state; the BE owns all compute.

## The boundary

```
  PLC / line ◀── SafeStateSink ──┐ enter_safe_state(reason+forensics) on BE death
                                 │ clear_safe_state() on healthy resume
   xinsp-fe.exe  (supervisor)    │
     spawn → monitor → on death: safe-state + read crash report → respawn (rate-limited)
        │ CreateProcess + Job Object (kill-on-close)
        ▼
   xinsp-backend.exe  (compute core)
     --project/--script/--autostart-fps → self open → compile → start; all plugins
     in-process; fatal crash → minidump + JSON crash report, then exit
```

- **BE = compute.** All plugins + the inspection script, in-process. A fatal fault
  writes a minidump + JSON crash report (`SetUnhandledExceptionFilter`) and exits.
- **FE = lifecycle + safety.** No image processing, never loads a plugin. Spawns
  the BE under a Job Object (kill-on-close), watches it, drives the line safe on
  death, respawns (rate-limited).

## SafeStateSink — the PLC seam

`xi_safe_state.hpp` (portable C++; no Win32/OpenCV). On BE death the FE — which
survives the crash — drives the line to a known-safe state:

```cpp
enum class SafeStateReason { BackendExit, PortUnresponsive, BootTimeout,
                             RespawnLimitExceeded, SupervisorShutdown,
                             CommsLost /* retained but inert since the comms gateway was removed */, ... };
struct SafeStateEvent { reason; backend_rc; exception_name; faulting_module;
                        last_phase; report_path; dump_path; custom_payload; ts_ms; };
class SafeStateSink { void enter_safe_state(const SafeStateEvent&);
                      void clear_safe_state(); const char* name(); };
```

`--safe-state=SPEC` picks the sink:
- **`log`** (default) — records the transition to the FE log.
- **`tcp:HOST:PORT` / `udp:HOST:PORT`** — `PlcSafeStateSink`: a newline-delimited
  JSON command to the PLC on enter/clear, carrying the crash forensics. UDP
  repeats the safety-critical `enter` (loss-tolerant); TCP does a short-timeout
  per-message connect (a down PLC can't stall the monitor). Schema isolated in
  `build_enter()/build_clear()` — adapt the keys to your PLC. Adding another
  transport (Modbus, OPC-UA, digital-out) is just another sink behind the factory.

A comms plugin in the BE can also register an emergency payload via
`host->set_safe_state(payload)`; the FE forwards it on a BE crash. Verified by
`examples/plc_safe_state/`.

## Backend headless autostart

So the FE needs no WS client (there is no C++ WS client — the server is
server-only), the BE self-runs a project from the command line: after binding its
WS port, `service_main` synthesizes the same wire commands a client would send.

| Flag | Effect |
|---|---|
| `--project=DIR` | `open_project` at boot |
| `--script=PATH` | script to `compile_and_load` (default: `project.json` `script`) |
| `--autostart-fps=N` | if N>0, `start` continuous mode |

The WS port stays open so a UI can attach later; reply frames are no-ops while no
client is connected.

## Crash history + UI modes

- **FE owns the crash-history timeline** (respawn events, reasons, forensics over
  time); **BE owns the per-crash artifacts** (minidump + JSON report). Reading and
  symbolicating a report is in [`../guides/debug.md`](../guides/debug.md).
- **VS Code modes** (`resolveBackendMode`): *managed* — the extension spawns the
  FE which spawns the BE; *attach* — connect to an already-running BE; *auto* —
  decide by what's reachable. The target is FE-as-single-supervisor with UIs
  spawning the FE and attaching.

## See also

- [`../guides/debug.md`](../guides/debug.md) — crash reports + minidumps + attach.
- [`../guides/deploy.md`](../guides/deploy.md) — production boot order + bundle.
- `xi_safe_state.hpp` / `xi_safe_state_plc.hpp` / `fe_main.cpp` — sources.
