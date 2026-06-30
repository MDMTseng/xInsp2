# `expose` plugin + script-output transport (design)

**Status:** design **converged 2026-06-30, ready to implement**. Graduates to `internals/` on ship.
**Supersedes:** the `preview` plugin naming + its image-only subscription model.
**Driver:** the v9 `vars`-core removal left "how does arbitrary script data get out?" answered only by an awkwardly-named, asymmetric path. See the integrator friction report (`plugins/docs/xinsp2_issue_doc/2026-06-30-v9-vars-removal-friction.md`, F-1/F-2/F-6) for the concrete DX evidence.
**Related:** `internals/dispatch.md` (emit_record + ordered sinks + `$seq`), `roadmap/run-result.md` (the single verdict), `internals/comms-sidecar.md` (the sidecar last-word model), `roadmap/webui-and-ui-export.md`.

---

## 1. Problem

`VAR(name, expr)` used to be the universal, core-owned, zero-setup way to surface **any** named value from **anywhere** in a script to the outside (~85 scripts, dynamic per-part names). The 2026-06 `refactor/remove-var-core` deleted the core value store; `VAR`/`EMIT` are now compile-only no-ops. The replacement — push a `xi::Record` to the `preview` plugin, read it back by pulling `exchange("get")` — exists and works, but:

- it's named **`preview`** (UI-flavoured) for what is really the general "script data-out" surface — a category error;
- the transport is **asymmetric**: images stream as a subscribe-gated push (per image *name*), but scalar **values are pull-only** (no value-subscribe; the old `vars` push was removed). A live values dashboard must poll;
- the shipped binary push is currently undecodable by stock clients (the F-6 XPV1-vs-old-header drift);
- there's no single, clearly-named "this is where output goes" that a new integrator finds.

## 2. Decision (the converged model)

**Core owns no data-out and no last-word.** Output is plugin composition over core's thin plumbing (`use()` → sink, ordered `$seq`, the single `run_result` verdict). Two sink roles, one transport shape, with the line-safe last-word living out-of-process:

| Concern | Owner | Notes |
|---|---|---|
| Surface arbitrary script values + images for consumers to read (the VAR replacement, official tool) | **`expose` plugin** | passive publish/expose; many consumers; renamed from `preview` |
| Actively push to an external peer + drive the line safe on death | **comm plugin** | same sink family + peer protocol + its **own spawned sidecar process** (liveness-watch → last-word). No core. |
| Single per-run pass/fail verdict | **core** | `xi::result()` → `run_result` event |
| The plumbing (record → sink, ordered flush, verdict) | **core** | nothing else |

DIY is always open — a user can write their own sink. The platform just ships `expose` as the default data-out starting point.

### Two consumer scenarios `expose` serves
1. **VSCode plugin-dev environment** — the developer views exposed data live through the **webui** panel. Human-facing, low-frequency, must be easy to render.
2. **Flow-as-app-core** — when the whole inspection flow is embedded as an application's core, an **external program** needs the internal data out. Machine-facing, possibly higher-frequency, needs a stable wire format.

Both ride the *same* channel/subscribe mechanism; only the consumer differs.

### Why `expose` (not `export`/`preview`/`var`)
`expose` names the actual role: **passively make data available for consumers to read.** `export` was rejected because it connotes *active outbound to a destination* — that is the **comm** plugin's job, and reusing the word would blur the two roles. `preview` was UI-flavoured. `var` was value-only-sounding (the surface also carries images).

## 3. The `expose` plugin

### Script side
```cpp
#include <xi/xi_expose.hpp>          // was xi/xi_preview.hpp

xi::Record r;
r.set("score", part.score);          // scalar values, in call order
r.set("count", n);
r.image("edges", edgeImage);         // image, tagged by key
xi::expose::send("lane", r);         // channel "lane" (string, implicit)
```
- **`PVAR` is removed.** The script just builds a plain `xi::Record` and sends it; the record *is* the payload. **Display order = the record's own key order** (yyjson preserves insertion order), so the old `__LINE__`/`$layout` ordering machinery is dropped too.
- `xi::expose::send(channel, rec)` / `xi::expose::Sink` (rename of `xi::preview::*`).
- `expose`'s `plugin.json` keeps `"sink": true`, so under `dispatch_threads>1` the host stages + flushes each `process(...)` in frame-arrival order (`$seq` = wire `run_id`). Live output never tears/reorders — the exact guarantee hardened in the 2026-06 ordered-sink work.

### Channels
Output is organised by **channel id** — a **string, created implicitly** on first `xi::expose::send("lane", r)` (no pre-declaration). A channel is the unit of subscription and of the UI tab. A run may write several channels (per stage / per camera / per thread). (Renamed from the preview "tab id" / `pg_id`.)

## 4. Transport — subscribe→push, pull also; one atomic frame

**A consumer subscribes by channel id; the backend pushes that channel's complete record on every run — but only to channels that have a subscriber.** No subscriber → nothing is pushed (subscription gating). Pull-latest stays available for on-demand readers.

```
consumer → cmd: subscribe   { channels: ["lane", "high"] }
consumer → cmd: unsubscribe { channels: ["lane"] }
  ← per run, for each SUBSCRIBED channel:  one binary frame (below), ordered by $seq
consumer → exchange_instance("expose", { command:"get", channel:"lane" })   // pull latest — returns the same frame
```

### The frame (one record = one atomic binary frame)
A complete record is delivered as **a single self-contained frame** — no cross-message reassembly. Chosen over a split value-event + image-frames precisely so a consumer always receives a whole record atomically.

```
[ magic "XEX1" ][ msgpack body ]

body = {
  v:       1,                       // version — decoder gate (kills the F-6 drift)
  channel: "lane",
  seq:     <run_id>,                // ordered-sink $seq, ordering/correlation
  json:    "<record serialized to a JSON string>",   // scalar values, original key order
  images:  [ { key: "edges", jpeg: <bin> }, ... ]     // each image JPEG-compressed
}
```
- **values:** the record's scalar tree is **dumped to a JSON string** and stuffed into the msgpack body — avoids a field-by-field yyjson→msgpack conversion; the decoder does a single `JSON.parse` / `json.loads`. Image keys in the record correspond to entries in `images[]`.
- **images:** **all JPEG**, carried as msgpack `bin`. JPEG is lossy / 8-bit gray|BGR — `expose` is deliberately the *lightweight, human/preview-oriented* surface. **Non-8-bit / lossless image transport is out of scope** — that belongs to a purpose-built plugin (DIY sink), not `expose`. (A future "raw this key" subscribe flag was considered and **deferred** — keep `expose` simple.)
- **pull latest** (`exchange get`) returns the **same frame**. `expose` keeps **exactly one latest frame per channel** (plugin state, not core).
- **Ordering:** the frame rides the ordered sink (`$seq`), so multi-worker dispatch can't reorder a channel's frames.
- **Cost note:** JPEG-encode is on the publish path, but subscription gating means it only runs when someone is actually subscribed — zero cost when nobody's watching.
- **Snapshot-on-subscribe:** **not done** (default). A late joiner is blank until the next run (or can `exchange get` the latest explicitly).

### Delta vs current
| | current (`preview`) | target (`expose`) |
|---|---|---|
| subscription unit | per image **name** | per **channel id** (string, implicit) |
| scalar values | **pull only** (+ the one `run_result` verdict) | delivered **inside the pushed frame** AND pullable |
| images | subscribe-push (name-gated, separate) | inside the same atomic frame (JPEG) + pull |
| payload framing | value-event + separate binary image frames | **one atomic frame** per record (`XEX1` + msgpack) |
| naming | `preview` / `tab` / `xi::preview` / `PVAR` | `expose` / `channel id` / `xi::expose` / (no PVAR) |
| binary frame | XPV1 producer, **stale decoders** (F-6) | XEX1 on producer **and** both stock decoders + version gate |

## 5. Last-word / line safety (no core, no this-plugin)

Not the `expose` plugin's job and not core's. A **comm plugin** that drives a PLC/line spawns its **own isolated sidecar process** that watches the backend's liveness (OS-level: wait on the process handle / a socket that breaks on death / a released file lock) and, on backend death, drives the line safe from its own still-alive PLC connection and sends the death telegram. This is more robust than any in-process "last word" (which a hard crash / `_Exit` / power loss would never run) and matches the existing `internals/comms-sidecar.md` direction (`set_safe_state` was already removed from core in 2026-06). Core provides **nothing** for this.

## 6. Migration (answers the integrator's F-1/F-2)

| was | now |
|---|---|
| `VAR(name, x)` / `xi::ValueStore::track(name, x)` (dynamic names) | `rec.set(name, x)` → `xi::expose::send(channel, rec)` |
| `EMIT(name)` | (no-op; surface via `expose`) |
| `client.next_vars()` (dead `vars` WS message) | subscribe a channel → receive the pushed frame; or pull `exchange("get", channel)`. Add a `subscribe_channel(channel)` / `expose_get(channel)` client helper. |
| image preview binary frame | inside the `expose` frame (XEX1, JPEG) or pull `get` |
| single verdict | unchanged: `xi::result()` → `run_result` |

> ⚠️ Do **not** redirect integrators to `expose` yet — it doesn't exist. The shipped name today is still `preview`; their current `xi::preview::send` migration is correct for now.

## 7. Rename + implementation change-set (when we build)

Mechanical but broad — `preview` → `expose`, `tab`/`pg_id` → `channel id`, drop `PVAR`, new atomic frame:
- `plugins/preview/` → `plugins/expose/` (+ `plugin.json` name).
- SDK `xi_preview.hpp` → `xi_expose.hpp`; `xi::preview::Sink`/`send` → `xi::expose::*`; **remove `PVAR`** (scripts build a plain `xi::Record`; drop the `$layout`/`__LINE__` ordering).
- Transport: subscription per-channel-id (string, implicit); **gate on having a subscriber** (no subscriber → no push); keep latest-per-channel for pull.
- Frame: emit the single `XEX1` + msgpack frame (`{v, channel, seq, json, images[]}`); JSON-string values; JPEG-encode each image on the publish path.
- F-6 / decoders: write the **`XEX1` decoder** in JS (`ui-components/src/protocol.mjs`) + Python (`tools/xinsp2_py/xinsp2/client.py`) — `JSON.parse`/`json.loads` the `json` field, JPEG-decode each image — with a magic/version gate, landed with a producer→JS→Python round-trip test.
- Clients: `subscribeImage(name)` → `subscribeChannel(channel)`; the webui consumer; `ws-protocol.md` (also correct the stale "subscribe/unsubscribe removed" note).
- The 2026-06 preview features carry over under the new name: **subscription gating** (send-none-until-subscribed) and **decode-once** (one JPEG decode per frame shared across components).
- Docs: `write-a-script.md` "Surfacing output", `reference/ws-protocol.md`, `roadmap/run-result.md` cross-ref; this doc → `internals/` on ship.
- `VAR`/`ValueStore` tombstone + `#pragma` deprecation (integrator F-1) lands alongside.

## 8. Non-goals / deferred
- **Peer protocols** (msgpack peer wire, multi-PLC, safe-state telegrams): the plugin maker's, per `comms-deferred`. Not in `expose`, not in core.
- **comm sidecar implementation**: separate design; out of scope here (this doc only fixes its boundary — it spawns its own process, no core hook).
- **No core value store returns.** `expose` holds latest-per-channel for pull; that state is the plugin's, not core's.
- **Raw / lossless / non-8-bit image transport** (and a per-key "raw" subscribe flag): deferred — complexity belongs to a purpose-built plugin, not `expose`.
- **Snapshot-on-subscribe:** not in v1.

## 9. Settled decisions (was: open questions)
1. **`PVAR`** — **removed** (not renamed). Scripts send a plain `xi::Record`; display order = record key order.
2. **Frame shape** — **one atomic `XEX1` + msgpack frame** per record (values as a JSON string, images as JPEG `bin`), *not* a split value-event + image-frames. Atomicity over piecewise rendering.
3. **Snapshot-on-subscribe** — **no** (default); late joiners pull `get` if they need the latest.
4. **Channel id** — **string, implicit** on first `send`.
5. **Push gating** — only channels with a subscriber are pushed.
6. **Image encoding** — **all JPEG**; non-8-bit/lossless is a different plugin's job.
7. **Magic** — **`XEX1`** (clean break from `XPV1`).
