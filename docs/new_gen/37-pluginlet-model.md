# 37 — the pluginlet model: parasitic native+UI reuse units

Status: **DESIGN + first reference impl** on `feat/reactive-uiview` (branched off
`polaris2_main`, 2026-07). The native gate/dedup core (`xi::Derived`,
`xi_reactive.hpp`), the `live-view` pluginlet's native half, and expose's viewport
relay are landed and tested. The overlay/report model (and everything in the
"DESIGN ONLY" list at the end) is settled design with **no code** — read the final
section before assuming any of it exists.

Revision 2 (2026-07-20) folded in a review pass that overturned three positions
from revision 1; see the **Corrections log**.

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

### The one real cost: two build worlds — and the TWO kinds of bridge

The native half compiles into the plugin DLL; the UI half bundles into the webui —
two pipelines, possibly two repos/teams. What keeps them from drifting depends on
WHAT the two halves must agree about, and there are two very different answers:

| | **data bridge** | **logic bridge** |
|---|---|---|
| the two halves exchange | **data** (frames down, control up) | **the result of the same computation** |
| example | `live-view` (push frames, receive viewport) | `dim_measure` (the teach overlay must land exactly where native measured) |
| consistency mechanism | a **contract file** both sides validate against — rides the existing canonical-msgpack golden-fixture rail (`test_mp_fixtures`) | **ONE kernel compiled twice** (native lib + WASM). There is no second implementation, so nothing can drift |
| cost | cheap; what `live-view` uses | a pure framework-free kernel sub-lib + emcc toolchain |

**Most pluginlets need only the data bridge.** The logic bridge is a NARROW escape
hatch — see the three-condition test below.

#### Reference implementation of the logic bridge: `plugins/geom_kernel`

`dim_measure` (sibling `xInsp` workspace) is the proven precedent:

- `plugins/geom_kernel` — a **pure, framework-free, no-throw, no-OpenCV** geometry
  kernel (`geok::Vec2`, robust fits, constructions, warp). Sources listed ONCE.
- flat **C ABI** (`export/geom_kernel_c`): `gk_compute(xy, n, descriptor_json,
  &out_json)` — Float32 point pool on the hot path, arbitrary-shaped results via a
  malloc'd JSON string (`gk_free`). Deliberately **not embind**.
- **ONE schema doc** (`export/GK_COMPUTE_SCHEMA.md`) is the single source of truth;
  both native and webui point at it and it is documented nowhere else.
- native: `dim_measure` links the kernel. wasm: the SAME kernel sources compiled
  under Emscripten → `ui/vendor/geom_kernel.wasm` + `.mjs`, consumed by
  `ui/components/geom-core.mjs`. Result: the teach preview is byte-identical to
  what the backend will measure ("WYSIWYM", zero backend-vs-UI drift).
- two disciplines worth copying: only the **portable compute** crosses to wasm (the
  `cv::Mat` image sampler stays native — the kernel does not sample, the backend
  fills that seam); and the **no-throw posture is matched** so native and wasm run
  the same code path. Missing wasm degrades gracefully (`isAvailable()` false → UI
  shows nominal, no crash).

#### When you actually need the logic bridge — all three must hold

> The UI needs an answer the backend **has not computed yet**, at **interaction
> speed**, that **must equal** what the backend will compute.

- "not computed yet" rules out everything that merely *displays* results (most UI).
- "interaction speed" rules out anything a round-trip can serve.
- "must equal" rules out anything where approximate-then-snap is acceptable.

Teaching a caliper passes all three (otherwise the operator teaches against a lie).
Genuine candidates beyond it are few: teach/calibration UIs generally, and
**non-rigid warp** coordinate mapping. Note rigid/affine does **not** qualify — ship
the resolved 3×3 and let the UI apply it.

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

## Three tiers — most plugins need NO UI half at all

Once overlays are declarative (below), the pluginlet requirement collapses for the
common case. Classify by what the UI must do, not by what the feature is:

| tier | needs | who |
|---|---|---|
| **1. no UI half** | native emits a **declarative overlay**; ONE generic renderer draws it | **most plugins** |
| **2. data-bridge pluginlet** | custom widget/interaction, but no shared computation | `live-view` (pan/zoom viewport) |
| **3. logic-bridge pluginlet** | the UI must compute what native will compute → shared kernel → WASM | `dim_measure` (teach preview) |

Do not climb a tier without the tier's justification. Tier 3 in particular is
earned only by the three-condition test above.

## Composing pluginlets: host fan-out, never peer-to-peer

The composition point is the **host plugin's `process()`** — that is the pluginlet
layer's `inspect.cpp`, and it stays imperative and visible, matching the project's
existing composition doctrine.

> **Each pluginlet owns ONE feature, reads only the host's source truth, and does
> not know any other pluginlet exists. The host fans one frame out to each.**

```cpp
void process(xi::PackIn& in, xi::PackOut& out) override {
    xi::Image img = in.image("frame");   // the single source truth
    view_.publish(img);                  // each gates itself; none knows the others
    hist_.publish(img);
    rec_.publish(img);
}
```

Peer-to-peer pluginlet messaging is **forbidden** — it rebuilds exactly the signal
graph this model was created to avoid. When B needs A's output, the HOST threads it
in explicitly (`auto m = measure_.compute(img); view_.publish(img, m);`) so the
dependency is one visible line of host code, not a hidden edge. When two pluginlets
need the same expensive artifact, do not share it sideways — lift it to a single
UPSTREAM truth the host computes once and passes to both ("own each truth once").

A thin common interface (`publish()` + `stats()`) is worth abstracting ONLY for
mechanical lifecycle/observability (so a host can report all its pluginlets' stats
under its own owner name). Data wiring never enters that interface. For 2–4
pluginlets, plain members are cleaner — do not abstract early.

## Lifecycle: a pluginlet is just a host member

A pluginlet needs no lifecycle machinery of its own — it is a member of the host,
constructed and destroyed with it (the identity-borrowing of §"parasitic", rendered
in C++):

```cpp
class MyPlugin : public xi::Plugin {
    xi::pluginlet::LiveView view_;
public:
    MyPlugin(const xi_host_api* h, const std::string& n)
        : xi::Plugin(h, n), view_(h, "ui/" + n) {}
    // no dtor needed: view_ dies with the host
};
```

The host's own ctor/dtor are the real lifecycle hooks (expose registers
`xi.ui.sink` in its ctor and unregisters in its dtor, `expose.cpp:53–68`). Two
traps for anything a pluginlet or host acquires:

- **capability registration is legal only from lifecycle code** (create / set_def /
  prepare / commit / exchange / destroy) — the factory ctor qualifies; a data-plane
  door or a cap handler does not (`XI_CAP_REG_ECONTEXT`).
- **the dtor also runs on REINIT**, not just on user delete. A plugin with
  `on_fault:"reinit"` is destroyed and rebuilt on a fault, so ctor/dtor must be
  strictly symmetric or a fault leaks or double-registers. (The framework's
  `unregister_all_for(owner)` sweep is a backstop, not the normal path.)

## Overlay is a projection, not a report field

The natural instinct — "let the plugin declare how to draw, and put that in the
report" — is wrong twice. Both wrongs were found in review; the conclusion below is
what survived.

**Wrong 1 — overlay in the report.** The report/record is inspection TRUTH: it is
stored, replayed, SPC'd. Drawing instructions are presentation; putting them there
pollutes the record permanently and forces every frame to carry them whether or not
anyone is looking. (`geom_kernel`'s `out_json` shows the real mixing today: its
`overlay:{}` blends result restatement — `virtual_line/circle/point` — with pure
diagnostics — `search_roi`, `caliper:{scan_dirs,centers,band,width}`.)

**Wrong 2 — overlay attached to the image.** Attractive because an overlay's
coordinates are the image's pixel space. But **images are shareable**: pool blobs
are refcounted and zero-copy, one blob is adopted by many packs and consumers. An
annotation glued to a shared subject leaks into every context that references it.

> **Principle: the annotation references the subject; the subject must never know
> its annotators.** The image is shared and must stay ignorant. The unit that owns
> one analysis is the **pack**.

**The conclusion: the pack splits in two.**

```
pack {
  report:  { …plugin-specific inspection truth, ANY shape… }   ← UI never parses it
  overlay: [ { target: "frame",    prims: [ …core vocabulary… ] },
             { target: "roi_crop", prims: [ … ] } ]            ← UI understands ONLY this
  frame:   <xi/image blob>                                      ← shared, untouched
}
```

- **`report` shape is per-plugin and opaque to the UI.** This is why a "generic
  renderer walks the report" scheme fails: plugins do different things, so report
  structures genuinely differ and no generic UI can interpret them. The uniform
  thing is not the report — it is the **overlay vocabulary**.
- **`overlay` entries name their subject** via `target` (a pack image-entry name),
  resolving multi-image packs (multi-camera, source + crop) while the reference
  points overlay → image, never image → overlay.
- **the overlay vocabulary is defined ONCE in core** (point / line / polyline /
  circle / arc / rect / text + verdict-driven styling), not invented per plugin.
  This is the single uniform point the whole scheme rests on.
- **transport stays byte-blind** — expose forwards, never interprets.

**Why this makes demand-gating structural.** `report` is produced unconditionally
(it is truth). `overlay` is produced ONLY when someone is watching — the whole
section is gated by the pluginlet's subscription gate, so an unobserved run never
even assembles it. Truth vs projection stops being a naming convention and becomes
the pack's structure.

**The honest cost.** Overlay generation moves INTO the plugin (only the plugin knows
what its own results mean — the direct consequence of report shapes differing). So
replaying a record with overlays means re-running the plugin's projection, not
re-deriving it UI-side. That is fine when replay re-runs inspection anyway; if
offline overlay-without-re-run is ever required, storing overlay in the record
becomes a deliberate, eyes-open trade — not the default.

## Corrections log (what this doc got wrong first)

Recorded because the discarded positions are attractive and will be re-proposed:

1. **"one kernel compiled to native+wasm is the general answer to two build worlds"**
   — over-generalized. It is the answer for the **logic bridge only**, which is rare
   (three-condition test). Most pluginlets need the data bridge, where a contract
   file suffices. *Overturned by:* the wasm in `dim_measure` exists narrowly to stop
   the teach UI and the plugin from having two geometry implementations.
2. **"declare type→drawing in the manifest and let a generic renderer walk the
   report"** — assumes report structures are uniform enough to interpret generically.
   They are not; plugins do different things. *Overturned by:* report heterogeneity.
   The uniform vocabulary must live in the overlay, not be inferred from the report.
3. **"attach overlay to the image (keyed by image entry)"** — images are shareable
   refcounted pool blobs; an annotation on a shared subject leaks to every consumer.
   *Overturned by:* image shareability. Corrected to the `report`/`overlay` pack split.

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
webui build (see "two build worlds").

### DESIGN ONLY — deliberately not built (CT, 2026-07)

The following are settled design in this doc with **no code written**; do not read
the sections above as describing shipped behaviour:

- **the `report` / `overlay` pack split** and the `target`-referenced overlay —
  agreed shape, not implemented. Nothing in the pack contract changed.
- **the core overlay vocabulary** (point/line/polyline/circle/arc/rect/text +
  verdict styling) — not defined anywhere in code yet. This is the prerequisite for
  tier-1 (no-UI-half) plugins and the highest-leverage next step, since it is the
  shared denominator every plugin would draw through.
- **the generic overlay renderer** in the webui — not written.
- **the thin `Plet` interface** (`publish()` + `stats()` for uniform lifecycle /
  metrics reporting under the host's owner name) — not written; plain members remain
  the recommendation until several pluginlets exist.

The one known gap in what IS built: `live_view.ui.ts` infers full-image dimensions
from the first frame, but the native half may already have downsampled it
(`max_edge`), so the widget's viewport coordinate basis can be the downsampled size
rather than true full-image pixels. Fixing it means the producer reporting true
full-image `w,h` alongside the frame. The UI half is an unbuilt/untested TS sketch;
only the native half and the expose relay are compiled and tested.
