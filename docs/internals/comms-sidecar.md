# The comms sidecar — line safety as a plugin's own process

> **Status: design / pattern.** This replaces the removed FE-brokered PLC
> safe-state (the `set_safe_state` ABI verb + the `--safe-state` FE sink, both
> removed 2026-06). It is the recommended shape for any plugin that has to drive
> a PLC / production line to a safe state when the inspection backend dies.

## The problem

A vision station usually has to tell the line controller (PLC) "stop / go to a
safe state" the instant the inspection software stops being trustworthy — most
critically when the **backend process itself dies** (a plugin crash, an OOM, a
power blip on the host). The hard part is exactly that case: the thing that would
send the message is the thing that just died.

Earlier designs put this in the core (a `set_safe_state` ABI verb the host
forwarded) and then in the FE supervisor (a `--safe-state` sink). Both were
removed because they coupled a **deployment-specific, protocol-specific** concern
(which PLC, which fieldbus, which safe-state telegram) into the generic core /
supervisor, and neither could honestly guarantee delivery *after the process
carrying it had crashed*.

## The pattern

The plugin that owns the PLC link **spawns its own sidecar process** and talks to
the PLC *from the sidecar*, not from inside the backend:

```
  ┌─────────────────────────────┐         ┌──────────────────────┐
  │ xinsp-backend.exe           │         │ comms sidecar (child)│
  │   comms plugin (in-proc)    │ spawn   │  - holds PLC link     │
  │     - opens sidecar  ───────┼────────▶│  - watches BE handle  │
  │     - streams live data ───►│  pipe   │  - on BE death:       │
  │       (results, heartbeat)  │◀────────┤      signal PLC SAFE  │
  └─────────────────────────────┘         │      then exit        │
            │ dies (crash/OOM)            └──────────▲───────────┘
            └─────────── OS closes handle ───────────┘
                         (sidecar's wait wakes)
```

1. **The comms plugin spawns a sidecar child process** on load (or first use).
   The sidecar — not the backend — holds the actual PLC connection (TCP / serial
   / fieldbus). The plugin streams live inspection data to it over a pipe / local
   socket during normal operation.
2. **The sidecar watches the backend's process handle.** On Windows it waits on
   the parent `HANDLE` (or a `PROCESS_QUERY_LIMITED_INFORMATION` handle / a job
   object); on Linux it uses `prctl(PR_SET_PDEATHSIG)` or a `pidfd`. This wait is
   the safety primitive: it fires whether the backend exits cleanly, crashes, or
   is killed, because the OS closes the handle either way.
3. **On backend death the sidecar drives the line safe itself**, using its own
   still-alive PLC connection — then sends its shutdown/death telegram and exits.
   No part of the dying backend has to run code for this to happen.

## Why a separate process (not a thread, not the FE)

- **It survives the crash it's reporting.** A thread in the backend dies with the
  backend. A separate process with its own PLC socket does not — its `wait` on the
  parent handle is the *only* thing that needs to still be running, and the OS
  guarantees that.
- **Protocol stays in the plugin's world.** The PLC dialect, the safe-state
  telegram, retry/timing behaviour, and which fieldbus library to link are all the
  plugin author's problem, kept out of the core and the FE. The core stays
  schema-agnostic; the supervisor stays a generic restart-and-record loop.
- **No core/FE coupling to undo per deployment.** Different lines use different
  PLCs. Baking one into the supervisor was wrong; a plugin-owned sidecar lets each
  deployment ship its own.

## How it relates to the FE supervisor

The FE (`xinsp-fe`) and the comms sidecar are **independent** safety nets with
different jobs:

| | FE supervisor | comms sidecar |
|---|---|---|
| Owner | the framework | the comms plugin author |
| Job | restart the backend, record crash forensics (`xi_crash_history`), expose `fe_status` | drive the **PLC / line** to a safe state on BE death |
| Reacts to | BE death → respawn (rate-limited), latch `down` at the cap | BE death → PLC safe telegram, then exit |
| Knows about the PLC? | **No** | **Yes** |

The FE's `fe_status` state goes to `down` on backend death (it no longer has a
`safe` state — that vocabulary moved out with the PLC delivery). Whether the
physical line actually went safe is the sidecar's responsibility and is reported
through the PLC, not through `fe_status`.

## Lifecycle notes for plugin authors

- **Spawn early, once.** Open the sidecar when the plugin loads so the PLC link is
  already live before the first inspection — you don't want to be establishing it
  during a crash.
- **Heartbeat the data path, not the safety path.** Stream results / a heartbeat
  to the sidecar over the pipe. If the pipe goes quiet the sidecar can choose to
  go safe on a timeout too (wedge detection), independent of the hard
  process-handle trigger.
- **Make the safe action idempotent.** The sidecar may be triggered by both a
  heartbeat timeout and the handle-close; sending the safe telegram twice must be
  harmless.
- **Exit after signalling.** Once the backend is gone and the line is safe, the
  sidecar's job is done — send its death message and exit so a fresh backend (the
  FE will respawn one) can spawn a fresh sidecar.
- **Cross-platform:** the handle-watch is the one OS-specific piece — `WaitForSingleObject`
  on the parent handle (Windows) vs `PR_SET_PDEATHSIG` / `pidfd_open` (Linux) /
  `kqueue` `EVFILT_PROC` (macOS). Keep it behind a small shim; everything else
  (the PLC protocol, the pipe) is portable.

## See also

- `docs/internals/fe-be.md` — the FE supervisor (the *other* safety net).
- `docs/archive/comms-gateway.md` — the removed in-core comms gateway (historical).
