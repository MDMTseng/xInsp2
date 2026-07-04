# PLC / external comms — a plugin concern (the gateway was removed)

> **Status: the out-of-process `xinsp-comms` gateway + the in-process `xi::comms`
> client + script API were REMOVED.** PLC / external I/O is now a plugin concern,
> and the "tell the PLC on a backend crash" guarantee is served by a comms
> plugin's own crash-watching sidecar process (see ../internals/comms-sidecar.md).
> This supersedes the old gateway design (Increments 1–3), and also the
> intermediate `set_safe_state` ABI verb + FE `--safe-state` sink, both of which
> shipped and were themselves removed 2026-06 — do not use them.

## Why the change

The original gateway lived out-of-process precisely so flaky, externally-driven
I/O (socket timeouts, malformed PLC frames, reconnect storms) couldn't hang or
crash the safety-critical FE or the in-process BE. That reasoning is sound — but
the gateway, its backend client, the `xi::comms` script API, and the FE's
supervision of it added a lot of *core* surface for something the project's
position says is **the plugin maker's problem** (the wire format / PLC schema is
not the framework's concern). So comms moved out of core entirely.

## The replacement

Two independent needs, each with a home that already existed:

### 1. Normal PLC I/O → a plugin

A plugin owns the PLC socket and speaks whatever protocol the line needs. It is a
normal `xi::Plugin`; nothing special. Under parallel dispatch, ordering of PLC
writes is handled by the plugin: it keeps N independent ordered streams keyed by
a stream id, and reorders each by an emitter-assigned `seq` (carried in the
frame's `dataInfo`). The producer guarantees a contiguous `seq` stream (strict
no-drop, or a blank for any seq it skips), so a stream never stalls.

See the reference templates under `examples/_diag/`:
- `comm_proj/` — the sort-comm reorder plugin (stream pools + seq order).
- `parallel_comm_proj/` — the same under a real 4-thread lane (scrambled in,
  ordered out).
- `compose_proj/` — camera → collector → inspection → comm, end to end.

A plugin dies with the backend, so it reconnects to the PLC on a respawn (the old
gateway kept the link alive across BE restarts; that persistence is the tradeoff
given up here — acceptable since comms is the plugin maker's domain).

### 2. "Backend crashed → tell the PLC" → the FE survivor

The FE outlives the backend by design and already owns a PLC-capable safe-state
sink (`--safe-state=tcp:HOST:PORT` / `udp:HOST:PORT`, see `xi_safe_state_plc.hpp`).
A plugin registers its emergency payload via the host call:

```cpp
host->set_safe_state("{\"cmd\":\"estop\"}");   // register / update; "" clears
```

The backend persists that payload atomically to `<project>/.xinsp_safestate`. On a
backend death the FE reads it and forwards it to the PLC inside the safe-state
message (`"payload"` field). This keeps the crash-safety guarantee without any
resident comms in core. Verified by `examples/_diag/safestate_smoke.py`.

## What was removed

- `xinsp-comms` executable (`backend/src/comms_main.cpp`) + its CMake target.
- `xi_comms.hpp` (the `xi::comms::*` script API) and `xi_comms_gateway.hpp` (the
  backend-side Winsock client `GatewayClient`).
- The backend's comms callbacks + `--comms-port`; the script-loader's
  `set_comms_callbacks`; `xi_script_set_comms_callbacks`.
- The FE's gateway spawn/supervise (`--comms-plc`/`--comms-port`/`--comms-exe`/
  `--comms-log`, `spawn_gateway`, the gateway respawn/heartbeat loop).
- Examples `comms_gateway/`, `comms_script/`, `fe_comms/`, and the comms-coupled
  `qa_soak/`.

The FE's `make_plc_sink` safe-state path (e.g. `examples/plc_safe_state/`) STAYS —
it is the basis of the crash-payload mechanism above.
