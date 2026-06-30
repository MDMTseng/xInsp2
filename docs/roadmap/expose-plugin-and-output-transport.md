# `expose` plugin + script-output transport (design sketch)

**Status:** design, 2026-06-30. Not yet implemented. Graduates to `internals/` on ship.
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

### Why `expose` (not `export`/`preview`/`var`)
`expose` names the actual role: **passively make data available for consumers to read.** `export` was rejected because it connotes *active outbound to a destination* — that is the **comm** plugin's job, and reusing the word would blur the two roles. `preview` was UI-flavoured. `var` was value-only-sounding (the surface also carries images).

## 3. The `expose` plugin

### Script side (unchanged shape, renamed namespace)
```cpp
#include <xi/xi_expose.hpp>          // was xi/xi_preview.hpp

xi::Record r;
for (auto& part : parts)
    PVAR(r, part.name.c_str(), part.score);   // dynamic per-part names; values AND images
PVAR(r, "edges", edgeImage);                  // image auto-tagged
xi::expose::send("lane", r);                  // group id "lane"  (was xi::preview::send / Sink)
```
- `PVAR(rec, key, data)` is unchanged (appends value/image in call order, stamps `__LINE__` for `$layout` ordering). **Open nit:** the `P` (was "preview") is now orphaned — keep `PVAR` (retcon "Publish a VAR", zero churn) vs rename to `XVAR`/`EVAR`. Recommend **keep `PVAR`**.
- `xi::expose::send(group, rec)` / `xi::expose::Sink` (rename of `xi::preview::*`).
- `expose`'s `plugin.json` keeps `"sink": true`, so under `dispatch_threads>1` the host stages + flushes each `process(...)` in frame-arrival order (`$seq` = wire `run_id`). Live output never tears/reorders — the exact guarantee hardened in the 2026-06 ordered-sink work.

### Grouping
Output is organised by **group id** (renamed from the preview "tab id" / `pg_id`). A group is the unit of subscription and of the UI tab. A run may write several groups (per stage / per camera / per thread).

## 4. Transport — one model: subscribe→push, pull also

**Subscribe by group id → the backend actively pushes that group's full payload (group id + values + images) on every run; pull remains available.** This unifies the two halves that are split today.

```
consumer → cmd: subscribe { groups: ["lane", "high"] }
  ← per run, for each subscribed group:  push { group, values{...}, images[...] }   (ordered by $seq)
consumer → exchange_instance("expose", { command:"get",       group:"lane" })   // pull values (on-demand / MES)
consumer → exchange_instance("expose", { command:"get_image", group:"lane", key:"edges" })  // pull a still
```

- **Subscribe granularity:** per **group id** (today: per image *name*).
- **Values:** now **pushed** to subscribers (today: pull-only). Symmetric with images.
- **Images:** pushed in the same subscription (today: already pushed, but separately and name-gated). The binary image frame keeps the `emit_binary` path; its header is fixed as part of F-6 (below) so stock clients decode it.
- **Pull** (`get` / `get_image` by group) stays as the on-demand / request-response path — what the current integrator chose (no subscribe dependency).
- **Ordering:** all pushes ride the ordered sink (`$seq`), so multi-worker dispatch can't reorder a group's frames.

### Delta vs current
| | current (`preview`) | target (`expose`) |
|---|---|---|
| subscription unit | per image **name** | per **group id** |
| scalar values | **pull only** (+ the one `run_result` verdict) | **pushed** to subscribers AND pullable |
| images | subscribe-push (name-gated) + pull | subscribe-push (group) + pull |
| naming | `preview` plugin / `tab` / `xi::preview` | `expose` plugin / `group id` / `xi::expose` |
| binary frame | XPV1 producer, **stale decoders** (F-6) | XPV1 on producer **and** both stock decoders + version gate |

## 5. Last-word / line safety (no core, no this-plugin)

Not the `expose` plugin's job and not core's. A **comm plugin** that drives a PLC/line spawns its **own isolated sidecar process** that watches the backend's liveness (OS-level: wait on the process handle / a socket that breaks on death / a released file lock) and, on backend death, drives the line safe from its own still-alive PLC connection and sends the death telegram. This is more robust than any in-process "last word" (which a hard crash / `_Exit` / power loss would never run) and matches the existing `internals/comms-sidecar.md` direction (`set_safe_state` was already removed from core in 2026-06). Core provides **nothing** for this.

## 6. Migration (answers the integrator's F-1/F-2)

| was | now |
|---|---|
| `VAR(name, x)` / `xi::ValueStore::track(name, x)` (dynamic names) | `PVAR(rec, name, x)` → `xi::expose::send(group, rec)` |
| `EMIT(name)` | (no-op; surface via `expose`) |
| `client.next_vars()` (dead `vars` WS message) | subscribe a group → receive pushed `{values, images}`; or pull `exchange("get")`. Add a `expose_get(group)` / `subscribe_group(group)` client helper. |
| image preview binary frame | `expose` binary push (XPV1, decoders fixed) or pull `get_image` |
| single verdict | unchanged: `xi::result()` → `run_result` |

The integrator picked **pull** (on-demand inspection) and is unblocked today on the durable `exchange` channel; the subscribe-push path is the live-UI layer.

## 7. Rename + implementation change-set (when we build)

Mechanical but broad — `preview` → `expose`, `tab`/`pg_id` → `group id`:
- `plugins/preview/` → `plugins/expose/` (+ `plugin.json` name).
- SDK `xi_preview.hpp` → `xi_expose.hpp`; `xi::preview::Sink`/`send` → `xi::expose::*`; keep `PVAR`.
- Transport: change subscription from per-image-name to per-group-id; add the **value push** on subscribe (the new bit); keep pull.
- F-6: fix the JS (`ui-components/src/protocol.mjs`) + Python (`tools/xinsp2_py/xinsp2/client.py`) decoders to parse XPV1, add a magic/version gate, land with a producer→JS→Python round-trip test.
- Clients: `subscribeImage(name)` → `subscribeGroup(group)`; the webui consumer; ws-protocol.md (also correct the stale "subscribe/unsubscribe removed" note — image subscribe is live).
- The 2026-06 preview features carry over under the new name: subscription gating (send-none-until-subscribed) and decode-once (one decode per frame shared across components).
- Docs: `write-a-script.md` "Surfacing output", `reference/ws-protocol.md`, `roadmap/run-result.md` cross-ref, this sketch → `internals/` on ship.
- `VAR`/`ValueStore` tombstone + `#pragma` deprecation (integrator F-1) lands alongside.

## 8. Non-goals / deferred
- **Peer protocols** (msgpack, multi-PLC, safe-state telegrams): the plugin maker's, per `comms-deferred`. Not in `expose`, not in core.
- **comm sidecar implementation**: separate design; out of scope here (this sketch only fixes its boundary — it spawns its own process, no core hook).
- **No core value store returns.** `expose` holds latest-per-group for pull; that state is the plugin's, not core's.

## 9. Open questions to settle before/with implementation
1. `PVAR` macro name — keep (recommended) vs `XVAR`/`EVAR`.
2. Pushed-value wire shape — a `{type:"expose", group, values, images:[gid…]}` event correlated with the binary image frames by `group`+`gid`, vs one combined frame. (Lean: separate value-event + binary image frames, both `group`-tagged — reuses the existing `emit_binary` path, keeps values as readable JSON.)
3. Whether `subscribe` returns the current latest immediately (snapshot-on-subscribe) so a late joiner isn't blank until the next run. (Lean: yes — push the latest on subscribe.)
