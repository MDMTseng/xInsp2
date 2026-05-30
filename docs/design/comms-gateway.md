# Comms gateway — out-of-process I/O plugins

> **Status: design (not built).** Forward-looking; captures the boundary and
> protocol before any code, same as [`fe-be-split.md`](./fe-be-split.md).

## Why

The PLC / external-comms interface is still in flux — we don't want to harden it
into either the safety-critical FE or the in-process BE yet. Comms code is also
the *worst* candidate for in-process loading: it's I/O-bound and externally
driven (socket timeouts, malformed PLC frames, reconnect storms, blocking reads)
— exactly the code most likely to hang or crash in surprising ways.

So introduce a **second plugin class, isolated in its own process**:

| Class | Where | Why | Crash blast radius |
|---|---|---|---|
| **Compute** (cameras, detectors, ops) | in-process in the BE, zero-copy via pointers | perf — they pass big images | takes the BE down → FE catches → safe-state + respawn |
| **Comms / I/O** (PLC bridge, fieldbus, external services) | **standalone process** ("comms gateway"), small messages over a local socket | volatile + I/O-bound; iterate freely without hardening; isolate the hang/crash risk | takes only the gateway down → FE respawns + drives safe-state |

This is **not** a return to the removed SHM mesh: that was heavy because compute
plugins pass images (zero-copy). Comms moves tiny control messages, so the
gateway needs only a socket — no SHM, no hardened ABI.

## Shape

```
        PLC / fieldbus
            ▲  │   TCP/UDP JSON|msgpack (the evolving, un-hardened wire)
            │  ▼
   ┌─────────────────────┐
   │  comms gateway       │  owns the PLC connection; reconnect/retry lives here
   │  (standalone process)│
   └─────────────────────┘
        ▲ loopback (JSON lines / framed)
        │
   ┌────┴────┬───────────────┐
   │   BE    │   FE           │
   │ (script │ (safe-state    │
   │  I/O)   │  normal path)  │
   └─────────┴───────────────┘
   supervised by the FE (BE + gateway are sibling children)
```

- The **gateway owns the PLC socket** and all the messy reconnect/retry/parse
  logic. Rewrite it freely; nothing it does can crash the BE or FE.
- The **BE/script** uses it for bidirectional **inspection I/O** — read a trigger
  or recipe from the PLC, write results/verdicts back — via a thin host-side
  handle that just forwards over the loopback socket (e.g. `xi::io.send(obj)` /
  `xi::io.poll()`; exact API TBD).
- The **FE** is the supervisor: it spawns + monitors **BE and gateway as sibling
  children**, respawning each with the existing `RespawnTracker` policy.

## Safe-state: the gateway is the dead-man (primary), with layered fallbacks

The gateway **owns the PLC connection**, so it's the natural thing to tell the PLC
to go safe when the backend dies — and it detects that loss instantly, via its
own loopback connection to the backend dropping. This is a clean dead-man chain:

- **Backend registers an emergency payload** up front: `{"op":"set_deadman","line":"<emergency line to the PLC>"}`.
  The PLC-specific schema stays the backend's; the gateway just holds the line.
- **Backend crashes** → its loopback connection to the gateway drops **without a
  `bye`** → the gateway immediately sends the registered payload to the PLC for
  emergency handling. A clean shutdown sends `{"op":"bye"}` first, which disarms
  the dead-man (no false trip).
- **Gateway crashes** → the PLC sees *its own* TCP connection drop → the PLC can
  dead-man on its own (link-loss = unsafe).
- **FE crashes** → the Job Object reaps the backend + gateway → all connections
  drop → PLC dead-man.

So the safety signal is layered: explicit payload on a clean-detectable backend
crash, and TCP link-loss as the backstop at every level above. The FE's direct
`PlcSafeStateSink` (`--safe-state=tcp:/udp:`) remains as a **redundant/operator
path** (and for backends run without a gateway); the gateway dead-man is the
primary, lowest-latency route because it sits on the same loopback that dies with
the backend.

## Local protocol (BE/FE ↔ gateway)

Loopback TCP on a configurable port (`--listen`), newline-delimited JSON. The
gateway is **schema-agnostic about the PLC payload** — it relays opaque `line`
strings and only parses the small control envelope, so the PLC wire can evolve
freely. (msgpack framing later — same follow-up as the PLC sink.)
- client → gateway: `{"id":N,"op":"send","line":"<verbatim to PLC>"}`,
  `{"op":"set_deadman","line":"<emergency line>"}`, `{"op":"bye"}`, `{"op":"ping"}`
- gateway → client: `{"id":N,"ok":true|false[,"err":...]}` ;
  `{"event":"plc_in","line":"<verbatim from PLC>"}` (async) ;
  `{"event":"plc_up","up":true|false}` (link state)

**Built:** `xinsp-comms.exe` — the relay + dead-man, TCP auto-reconnect
(`examples/comms_gateway/`). And the backend-side **`xi::comms`** API
(`xi_comms.hpp`: `send` / `poll` non-blocking / `up` / `set_deadman`): the
backend connects with `--comms-port=N`, holds the gateway client + a background
reader buffering inbound, and installs the host callbacks into the script DLL
(same pattern as `xi::status`). Tested end-to-end in `examples/comms_script/`
(script↔PLC round trip + link state). Still to build: the FE supervising the
gateway as a sibling of the backend (Increment 3).

## Supervision & failure semantics

- FE spawns gateway with its config (PLC endpoint, loopback port). Job Object
  kill-on-close, same as the BE — no orphans.
- Gateway crash → FE respawns (rate-limited) + safe-state (line can't reach PLC).
- BE crash → unchanged (FE catches; gateway keeps running, holds the PLC link).
- Independent lifecycles: a BE recompile/respawn doesn't drop the PLC connection
  (the gateway holds it) — a nice side benefit.

## Explicitly out of scope

- **No images / zero-copy through the gateway.** Control messages only. Anything
  image-heavy stays in-process in the BE — routing big buffers across the process
  boundary is the SHM problem we deliberately removed.
- Hardened wire format / msgpack / multi-PLC fan-out — later.

## Open decisions (resolve when building)

1. Loopback transport: TCP vs named pipe / UDS. (Lean TCP — reuses code, cross-platform.)
2. Framing: newline-JSON vs length-prefixed. (Lean newline-JSON for v1.)
3. BE-side API surface: a script primitive (`xi::io`) vs a host_api call vs a
   special instance. (Lean a thin script primitive that forwards.)
4. Does the gateway also serve the FE's safe-state, or only BE I/O? (Lean BE I/O
   only; FE safe-state stays direct.)
5. Is the gateway a separate exe (`xinsp-comms`) or a mode of an existing one?

## Verification approach

A gateway **echo/sim** test (mirroring `examples/plc_safe_state`'s UDP PLC sim):
stand up a fake PLC, run FE+BE+gateway, drive a script that sends + polls via the
gateway, and assert round-trip; then crash the gateway and assert the FE respawns
it + drives safe-state, with the BE surviving.
