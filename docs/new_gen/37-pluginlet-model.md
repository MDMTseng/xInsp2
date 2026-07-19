# 37 — the pluginlet model: parasitic native+UI reuse units

Status: **DESIGN + first reference impl** on `feat/reactive-uiview` (branched off
`polaris2_main`, 2026-07). The native gate/dedup core (`xi::Derived`,
`xi_reactive.hpp`) is landed and tested (doc-less until now); this doc names the
pattern it belongs to and specifies the two-halves packaging (`live-view` is the
first concrete pluginlet).

## The observation (CT)

> 這功能其實也算是一個跟 plugin 模式相似的 lib，只是它寄生在 plugin 裡面。
> …UI 部分也可以有類似的模式嗎，這樣使用者就不用去 deal with 這些細碎邏輯。

Correct, and precise. `xi::Derived` / `UiView` (doc: `xi_reactive.hpp`) is the
**same thing as a cap lib, minus the plane** — it kept the discipline (gate,
dedup, lifecycle, stats) and dropped the infrastructure (funnel, ABI marshalling,
name registry, own fault domain). A **pluginlet** is that idea made first-class,
and extended to carry a UI half so a plugin author references ONE thing and gets
both the backend derivation and its frontend widget, wired.

## What a pluginlet is: borrow the host's identity, not your own

xInsp2 already has two ways to package reusable behaviour, distinguished by ONE
axis — **owner identity**:

| tier | plane | owner identity | fault domain | discovery |
|---|---|---|---|---|
| **plugin instance** | data | own (project graph) | own | project config |
| **cap lib** | cap | own (`ImagePoolOwnerId`) | own (quarantine/reinit) | name registry (`xi.cap`) |
| **pluginlet** | *neither* | **the host's** | **the host's** | compile-time `#include` / bundle |

Everything the cap plane spends effort on — `OwnerGuard(owner)` swapping the
attribution, SEH charged to the lib instance, `unregister_all_for(owner)` on
teardown, reentrancy-by-owner — is the cost of **issuing an identity card**. A
pluginlet declines the card: its allocations bill the host plugin, its crash IS
the host's crash, its lifecycle is the host's `create`/`destroy`. That is the
literal meaning of *parasitic* (CT's 寄生) — it borrows the host's identity
instead of holding its own. The cap plane's entire cost is exactly that one thing
a pluginlet gives up.

Why this is the right trade for a plugin's OWN preview: the preview is that
plugin's private policy; if it crashes it SHOULD be charged to that plugin; and a
per-frame hot path should not pay pack-marshalling + funnel dispatch + owner
resolve. Two questions decide pluginlet vs cap lib:

1. **Private to one plugin, or a contract between plugins?** private → pluginlet;
   named contract others depend on → cap lib.
2. **Needs its own fault domain / versioning / runtime hot-swap?** no → pluginlet;
   yes → cap lib.

Because the interface shape is identical, there is a **graduation path**: a
pluginlet's three functions (`demand`/`project`/`sink`) are a cap handler taken
apart — promote them to a registered handler when a feature outgrows "private".
Near-zero rewrite.

### The shape is not a coincidence

`xi::Derived`'s triple maps onto cap-plane mechanisms that ALREADY EXIST:

| `xi::Derived` (pluginlet) | cap lib (on the plane) |
|---|---|
| `demand()` → viewers | `$probe: true` (interest/versions, no work) |
| `input_hash` dedup | imgcodec's FNV-1a content cache |
| `stats{}` ledger | `CapMetrics` (`calls/errors/total_us`) |
| `project()` | handler body |
| `viewers<=0 → Suspended` | funnel gates (quarantine/reentrancy → refuse) |

The dedup and stats are the *same primitives* the cap plane keeps, lifted
in-process. That equivalence is the proof the observation is right: a pluginlet is
**a capability you didn't bother to export.**

## Two halves + a contract

```
pluginlets/live-view/
├── live_view.hpp        native: xi::pluginlet::LiveView (the Derived cell), #include'd into the host plugin DLL
├── live_view.ui.ts      ui:     mount(el, channel) → renders frames, emits viewport back
└── contract.(ts|json)   SHARED: channel naming, viewport message schema, control schema
```

Author-facing surface is two lines of C++ plus one manifest key:

```cpp
#include <pluginlets/live_view.hpp>
xi::pluginlet::LiveView view{host, name};   // native half, in the ctor
view.publish(img);                          // per processed frame
```
```json
// plugin.json
"pluginlets": ["live-view"]
```

The frontend reads that declaration and auto-mounts `live_view.ui.ts` bound to the
plugin's channel. The author touches **no** channel string, subscription probe, or
viewport message — the fiddly protocol is internal to the pluginlet.

### Delivery: header for native, its "header-equivalent" for UI

Each half has the same header-vs-loaded axis the native tier had, and the clean
(parasitic) choice on both is compile-time:

- **native half — a header.** `#include` into the host DLL. Compile-time, zero
  runtime infra, no versioning drama. This is `xi_reactive.hpp` today.
- **UI half — a self-contained TS module bundled into the webui at BUILD time**
  (the import model). Still compile-time binding, no runtime UI-plugin loader.
  The heavier alternative — expose dynamically serving the widget asset so a
  dropped-in plugin's UI appears with no webui rebuild — is the "real plugin"
  infra on the UI side; deliberately not bought first (matches the native tier's
  header-first instinct).

### The one real cost: two build worlds

The native half compiles into the plugin DLL; the UI half bundles into the webui —
two pipelines, possibly two repos/teams. "Reference it and it stays in sync" holds
only because the `contract` file is the single source of truth both sides validate
against. This rides an EXISTING discipline: the canonical-msgpack golden fixtures
(`test_mp_fixtures`) already keep C++/TS/Python codecs in cross-language lockstep.
A pluginlet's contract sits on that same rail — it is not a new cross-language
mechanism, just a new consumer of one.

## What the pluginlet uniquely unlocks: viewport feedback gets a home

`xi::Derived`'s `Demand::window` was aspirational (doc `xi_reactive.hpp`): no ABI
viewport channel exists, so `UiView` gates on subscription only. The pluginlet
closes this WITHOUT touching the frozen ABI, and it is now **landed end to end**.
The UI widget already knows the viewport (it is the thing displaying); it sends the
viewport back on the SAME channel (as expose's `viewport` exchange command); expose
stores it per channel (only while subscribed) and hands it back as the `viewport`
key in the xi.ui.sink probe reply; the native `LiveView::demand_()` reads it,
`Demand{viewers, window}` becomes real (window folds into the cell's dedup key so a
pan/zoom re-projects), and `project_()` crops+downsamples to exactly that window
before encoding. The viewport protocol is **pluginlet-internal**, not a new global
ABI channel — which is exactly why it ships without an ABI change. This is where
the pluginlet concept earns its keep: it converts an aspirational seam into a
shipped feature by scoping the protocol to the pluginlet. (Verified: cap_ui_egress_
test "VIEWPORT RELAY" section, through the real expose/imgcodec/ui_egress DLLs.)

## Naming

`pluginlet`, not `subplugin`. "sub" implies a nested plugin with its own
structure/identity; a pluginlet has neither (it borrows the host's). The `-let`
diminutive claims only "plugin-shaped, lighter" (booklet/applet), which is exactly
true. CT's 寄生 ("parasite") names the mechanism precisely but is not a good
product noun — `pluginlet` is the noun for the same idea.

## Status of the reference impl (`live-view`)

- **native core** — `xi_reactive.hpp` (`xi::Derived<Out>` + `UiView`), 9/9 unit
  tests (`test_reactive`, gate/dedup/stats + ordering law), UiView compiles against
  the real cap/pack ABI. Landed.
- **restructure into a pluginlet** — `UiView` → `xi::pluginlet::LiveView`;
  `pluginlets/live-view/` two-halves + contract + manifest; UI widget
  (subscribe + render + pan/zoom emit-viewport); `Demand::window` folded into the
  dedup key and wired to a crop+downsample. Landed.
- **viewport relay through expose** — expose stores the browser's per-channel
  viewport (only while subscribed) and returns it in the xi.ui.sink probe reply;
  `LiveView::demand_()` reads it. Landed; verified by cap_ui_egress_test's
  "VIEWPORT RELAY" section.

Remaining (not scheduled): the webui-side transport that maps the widget's
ViewportMessage onto expose's `viewport` exchange command and mounts the widget
from a plugin's `"pluginlets"` declaration — the frontend integration, owned by the
webui build (doc 37 "two build worlds").
