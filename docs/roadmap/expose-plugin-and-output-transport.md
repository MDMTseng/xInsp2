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

### Script side — no special header
`expose` is called like any other plugin: the generic `xi::use("expose").process(record)`. **There is no `xi_expose.hpp`** — a dedicated header would only wrap that one call, so it earns nothing. The channel rides in the record under the documented reserved key `"$channel"`.
```cpp
xi::Record r;
r.set("score", score);               // scalar values, in call order
r.set("count", n);
r.image("edges", edgeImage);         // image, tagged by key
r.set("$channel", "lane");           // channel id (string, implicit) — reserved key
xi::use("expose").process(r);        // generic plugin call
```
- **`PVAR` is removed** and **so is the header.** The script just builds a plain `xi::Record`; the record *is* the payload. **Display order = the record's own key order** (yyjson preserves insertion order), so the old `__LINE__`/`$layout` ordering machinery is dropped too.
- `"$channel"` is the only contract beyond the plugin name; the host also stamps `"$seq"` (= run_id) for ordering. The plugin strips both from the published `json`.
- `expose`'s `plugin.json` keeps `"sink": true`, so under `dispatch_threads>1` the host stages + flushes each `process(...)` in frame-arrival order (`$seq` = wire `run_id`). Live output never tears/reorders — the exact guarantee hardened in the 2026-06 ordered-sink work.

> **Plugin-owned script headers (the rule going forward).** `preview` shipped its script API as `backend/include/xi/xi_preview.hpp` — a core include for a *plugin's* contract. That was a layering mistake. The rule: if a plugin needs a script-facing header it lives in the plugin (`plugins/<name>/include/xi/…`) and the script compiler adds each loaded plugin's `include/` to its `/I` set. `expose` simply needs no header, so that mechanism isn't built yet — the first plugin that genuinely needs one implements it.

### Channels
Output is organised by **channel id** — a **string, created implicitly** on first `xi::expose::send("lane", r)` (no pre-declaration). A channel is the unit of subscription and of the UI tab. A run may write several channels (per stage / per camera / per thread). (Renamed from the preview "tab id" / `pg_id`.)

## 4. Transport — subscribe→push, pull also; one atomic frame

**A consumer subscribes by channel id; the plugin then JPEG-encodes + pushes that channel's complete record on every run — but only for channels that have a subscriber.** No subscriber → no encode, no push (subscription gating). Pull-latest stays available for on-demand readers.

### Where subscription lives (the architecture constraint)
The core's `binary_sink` is a **dumb byte pipe**: `emit_binary` is broadcast to every WS client; the core never inspects or routes by channel ("the frame format is the plugin's contract with its UI"). There is **no server-side subscribe**. So subscription is tracked **in the `expose` plugin**, driven over its `exchange` channel — not a new backend WS command. The plugin keeps a set of subscribed channels; `process()` only encodes + emits frames for channels in that set. Frames still broadcast on the wire; each client filters by the frame's `channel`. This satisfies the cost rationale (no subscriber → zero JPEG-encode) without breaking the dumb-pipe core.

```
consumer → exchange_instance("expose", { command:"subscribe",   channels:["lane","high"] })
consumer → exchange_instance("expose", { command:"unsubscribe", channels:["lane"] })
  ← per run, for each SUBSCRIBED channel:  one XEX1 binary frame (below), broadcast, ordered by $seq
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
| naming | `preview` / `tab` / `xi::preview` / `PVAR` | `expose` / `channel id` / `xi::use("expose")` / (no header, no PVAR) |
| binary frame | XPV1 producer, **stale decoders** (F-6) | XEX1 on producer **and** both stock decoders + version gate |

## 5. Last-word / line safety (no core, no this-plugin)

Not the `expose` plugin's job and not core's. A **comm plugin** that drives a PLC/line spawns its **own isolated sidecar process** that watches the backend's liveness (OS-level: wait on the process handle / a socket that breaks on death / a released file lock) and, on backend death, drives the line safe from its own still-alive PLC connection and sends the death telegram. This is more robust than any in-process "last word" (which a hard crash / `_Exit` / power loss would never run) and matches the existing `internals/comms-sidecar.md` direction (`set_safe_state` was already removed from core in 2026-06). Core provides **nothing** for this.

## 6. Migration (answers the integrator's F-1/F-2)

| was | now |
|---|---|
| `VAR(name, x)` / `xi::ValueStore::track(name, x)` (dynamic names) | `rec.set(name, x)`; `rec.set("$channel", ch)` → `xi::use("expose").process(rec)` |
| `EMIT(name)` | (no-op; surface via `expose`) |
| `client.next_vars()` (dead `vars` WS message) | subscribe a channel → receive the pushed frame; or pull `exchange("get", channel)`. Add a `subscribe_channel(channel)` / `expose_get(channel)` client helper. |
| image preview binary frame | inside the `expose` frame (XEX1, JPEG) or pull `get` |
| single verdict | unchanged: `xi::result()` → `run_result` |

> ⚠️ Do **not** redirect integrators to `expose` yet — it doesn't exist. The shipped name today is still `preview`; their current `xi::preview::send` migration is correct for now.

## 7. Rename + implementation change-set (when we build)

Mechanical but broad — `preview` → `expose`, `tab`/`pg_id` → `channel id`, drop `PVAR`, new atomic frame:
- `plugins/preview/` → `plugins/expose/` (+ `plugin.json` name).
- **Delete** `backend/include/xi/xi_preview.hpp` — no replacement header (scripts call `xi::use("expose").process(rec)` directly; channel via `rec.set("$channel", …)`; drop `PVAR` + the `$layout`/`__LINE__` ordering). Plugin-owned headers are the rule for any future plugin that needs one.
- Transport: subscription per-channel-id (string, implicit), tracked **in the plugin** via `exchange` `subscribe`/`unsubscribe` (NOT a backend WS command — core stays a dumb byte pipe); **gate the JPEG-encode + emit on having a subscriber**; keep latest-per-channel for pull. msgpack is **hand-rolled** (minimal, fixed-shape encoder/decoder, no new dependency on any of the 3 sides) — still valid msgpack wire format.
- Frame: emit the single `XEX1` + msgpack frame (`{v, channel, seq, json, images[]}`); JSON-string values; JPEG-encode each image on the publish path.
- F-6 / decoder: the **`XEX1` decoder** lives in the plugin's own webUI (`plugins/expose/ui/index.html`), NOT in any client lib — `JSON.parse` the `json` field, JPEG-decode each image, magic/version gate. (Final architecture §10: expose is a pure plugin; core/extension/ui-components/hmi/python carry zero expose code.)
- Subscription gating (send-none-until-subscribed) lives in the plugin (`exchange subscribe`).
- Docs: `write-a-script.md` "Surfacing output", `reference/ws-protocol.md`, `roadmap/run-result.md` cross-ref; this doc → `internals/` on ship.
- `VAR`/`EMIT` + `xi_var.hpp` **deleted** from core (hard removal, integrator F-1); legacy scripts must migrate to `xi::use("expose")`.

## 8. Non-goals / deferred
- **Peer protocols** (msgpack peer wire, multi-PLC, safe-state telegrams): the plugin maker's, per `comms-deferred`. Not in `expose`, not in core.
- **comm sidecar implementation**: separate design; out of scope here (this doc only fixes its boundary — it spawns its own process, no core hook).
- **No core value store returns.** `expose` holds latest-per-channel for pull; that state is the plugin's, not core's.
- **Raw / lossless / non-8-bit image transport** (and a per-key "raw" subscribe flag): deferred — complexity belongs to a purpose-built plugin, not `expose`.
- **Snapshot-on-subscribe:** not in v1.

## 9. Settled decisions (was: open questions)
1. **`PVAR` + the SDK header** — **both removed.** Scripts call `xi::use("expose").process(rec)` directly (channel via `rec.set("$channel", …)`); display order = record key order. No `xi_expose.hpp`.
2. **Frame shape** — **one atomic `XEX1` + msgpack frame** per record (values as a JSON string, images as JPEG `bin`), *not* a split value-event + image-frames. Atomicity over piecewise rendering.
3. **Snapshot-on-subscribe** — **no** (default); late joiners pull `get` if they need the latest.
4. **Channel id** — **string, implicit** on first `send`.
5. **Push gating** — only channels with a subscriber are encoded + pushed. Subscription is tracked **in the plugin** via `exchange subscribe/unsubscribe` (Option 1), since the core's `emit_binary` is a dumb broadcast pipe with no server-side routing. Frames broadcast; clients filter by `channel`.
6. **Image encoding** — **all JPEG**; non-8-bit/lossless is a different plugin's job.
7. **Magic** — **`XEX1`** (clean break from `XPV1`).

## 10. Where the code lives — expose is a PURE plugin (final architecture)

**Decision (supersedes the earlier "every client decodes XEX1" sketch):** `expose`
is a *pure, self-contained plugin*. The XEX1 decoder + channel rendering live **only
in the plugin's own webUI** (`plugins/expose/ui/index.html`, `has_ui:true`), hosted
generically by whatever already hosts plugin webUIs (the VS Code `get_plugin_ui` →
`<folder>/ui/index.html` path, or a direct WebSocket). The pre-v9 `vars` + `gid` +
per-image-name preview model is **removed**, and **no expose/preview/VAR code lives in
core, the vscode-extension, ui-components, hmi, or the Python client** — they are all
generic.

| Layer | What it carries about expose |
|---|---|
| **core** (backend) | **nothing** — generic `emit_binary` broadcast + `compress_image` + sink `$seq` ordering only. No `vars` wire, no `PreviewHeader`, no `xi_var.hpp`/VAR. |
| **`expose` plugin** | **everything**: the C++ sink (XEX1 encode, channel gating, latest-per-channel) **and** its own `ui/` webUI (self-contained XEX1 decoder + channel render). |
| **vscode-extension** | generic plugin-webUI host (`get_plugin_ui`); zero expose/preview/vars code. |
| **ui-components** | generic `xi-*` components + a generic `XiClient` (`onBinary` raw passthrough, no decode). |
| **hmi** | generic dashboard host; ignores binary frames. |
| **Python client** | generic WS client (`exchange_instance` talks to any plugin); no expose decode. |

**Wire contract (unchanged, the plugin's webUI implements it):**
- Decoded frame: `{ v:1, channel, seq, values:<parsed json>, images:[{key, jpeg/dataUrl}] }`.
- Subscription over the plugin `exchange` (never a backend WS command):
  `exchange_instance("expose", {command:"subscribe"|"unsubscribe", channels:[…]})`;
  `{command:"get", channel}` → `{found, channel, seq, frame_b64}`; `{command:"list_channels"}`.
- An external consumer (non-first-party) that wants the data just speaks the same
  exchange + decodes XEX1 itself — the format is public; the first-party tree stays clean.
