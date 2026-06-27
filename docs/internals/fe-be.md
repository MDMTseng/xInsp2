# FE / BE split — supervisor + crash history

**Shipped design-of-record.** Process isolation + the SHM mesh were removed
2026-05 — every plugin (cameras included) runs **in-process** in the backend (BE)
compute core. The trade was deliberate (*a dead plugin means a dead pipeline
regardless of isolation*), but a deployed line still needs something to keep the
backend running when the BE dies. That's the **frontend supervisor (FE)**,
`xinsp-fe.exe`: it owns the BE process lifecycle; the BE owns all compute.

The FE is a **pure restart-supervisor + crash-history recorder**. It does **not**
broker line safety — driving the PLC to a safe state on a BE crash is the job of a
**comms plugin's own sidecar process** (see [Line safety](#line-safety) below),
not the core or the FE.

## The boundary

```
   xinsp-fe.exe  (supervisor)
     spawn → monitor → on death: read crash report → record history → respawn (rate-limited)
        │ CreateProcess + Job Object (kill-on-close)
        ▼
   xinsp-backend.exe  (compute core)
     --project/--script/--autostart-fps → self open → compile → start; all plugins
     in-process; fatal crash → minidump + JSON crash report, then exit
```

- **BE = compute.** All plugins + the inspection script, in-process. A fatal fault
  writes a minidump + JSON crash report (`SetUnhandledExceptionFilter`) and exits.
- **FE = lifecycle.** No image processing, never loads a plugin. Spawns the BE
  under a Job Object (kill-on-close), watches it, reads the crash report on death,
  records crash history, respawns (rate-limited).

## Line safety

The FE no longer brokers PLC safe-state. Instead, a **comms plugin spawns its own
sidecar process** that holds the PLC link, watches the backend process handle, and
signals the PLC to go line-safe if the backend dies. Because the sidecar is a
separate process owning the PLC connection, it survives a BE crash and reacts
independently of the core and the FE — neither core nor FE is on the safety path.
See [`comms-sidecar.md`](./comms-sidecar.md) for the full pattern (lifecycle,
the handle-watch primitive, and cross-platform notes).

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

## Liveness detection (three layers)

A backend can fail without crashing — it can hang. The FE catches that with three
independent budgets, each watching a different boundary:

- **Boot hang** (`--boot-timeout-ms`, default 60 s): the BE bound its port but never
  reached `autostart: ready`. Respawn.
- **Serving-loop wedge** (`--heartbeat-stale-ms`, default 15 s): the BE writes a
  monotonic heartbeat from its serving loop; if it stalls while the port still
  accepts TCP, a synchronous WS handler has wedged `srv.poll()`. Respawn.
- **Dispatch-worker wedge** (`--watchdog-ms`, default 60 s): the serving-loop
  heartbeat keeps advancing even when a dispatch **worker** is deadlocked inside a
  plugin's `process()` — so on an unattended line the inspection could silently
  stop while the FE shows healthy. The FE arms the BE's per-inspect watchdog
  (`--watchdog=N`, off in the bare BE), which times each inspect on the worker
  itself (continuous workers + `cmd:run`); on overrun it cooperatively cancels then
  exits the process, and the FE respawns. Generous by default so a legitimately
  long frame isn't killed; `--watchdog-ms=0` disables it (e.g. a dev stepping a
  long inspect). The arming is the *supervised-deployment* fail-safe — the bare BE
  default stays off.

All three end in the same place: respawn, rate-limited by the consecutive-failure
cap, which latches `"down"` when exhausted.

## Crash history + UI modes

- **FE owns the crash-history timeline** (respawn events, reasons, forensics over
  time); **BE owns the per-crash artifacts** (minidump + JSON report). Reading and
  symbolicating a report is in [`../guides/debug.md`](../guides/debug.md).
- **`fe_status`** is the FE's status channel (`fe-status.json`): the value a UI
  reads instead of inferring state from a WS disconnect. On a respawn cap the
  status latches `"down"` carrying the death reason + forensics.
- **VS Code modes** (`resolveBackendMode`): *managed* — the extension spawns the
  FE which spawns the BE; *attach* — connect to an already-running BE; *auto* —
  decide by what's reachable. The target is FE-as-single-supervisor with UIs
  spawning the FE and attaching.

## See also

- [`../guides/debug.md`](../guides/debug.md) — crash reports + minidumps + attach.
- [`../guides/deploy.md`](../guides/deploy.md) — production boot order + bundle.
- `fe_main.cpp` / `xi_crash_history.hpp` / `xi_fe_status.hpp` — sources.
