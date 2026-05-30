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

## The safe-state boundary (important)

Safe-state is the last-resort safety command; its delivery must **not** depend on
the iterable gateway being up. So:
- **Safe-state stays the FE's direct, hardened path** — the `PlcSafeStateSink`
  we already built (`--safe-state=udp:/tcp:`). Tiny, reviewed, doesn't change.
- The gateway carries the **evolving bidirectional inspection I/O**, which is what
  we don't want to harden yet.
- **Gateway down is itself a safe-state trigger**: if the line can't reach the
  PLC, it's unsafe → the FE drives safe-state (via the direct sink) and respawns
  the gateway. (Belt-and-suspenders: the direct sink is also the fallback route
  for the safe-state command if one ever wanted to send it through the gateway.)

## Local protocol (BE/FE ↔ gateway)

Loopback TCP on a configurable port (reuses our socket code; the gateway already
speaks TCP/UDP). Newline-delimited JSON for v1 (msgpack later — same follow-up as
the PLC sink). Two message directions:
- **Request/response** (client→gateway): `{"id":N,"op":"send","payload":{...}}`
  → `{"id":N,"ok":true}` / `{"id":N,"ok":false,"err":"..."}`. `id` matches replies.
- **Async inbound** (gateway→client, no id): `{"event":"plc_in","payload":{...}}`
  — PLC-originated data pushed to the BE (e.g. a trigger).

Framing + the op set are the main things to pin down when we build; the schema of
`payload` is the PLC's, kept out of the core (same isolation as the PLC sink's
`build_*`).

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
