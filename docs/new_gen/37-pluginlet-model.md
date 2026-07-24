# 37 — the pluginlet model: parasitic native+UI reuse units

Status: **DESIGN + first reference impl** on `feat/reactive-uiview` (branched off
`polaris2_main`, 2026-07). The native gate/dedup core (`xi::Derived`,
`xi_reactive.hpp`), the `live-view` pluginlet's native half, and expose's viewport
relay are landed and tested. The overlay/report model (and everything in the
"DESIGN ONLY" list at the end) is settled design with **no code** — read the final
section before assuming any of it exists.

Revision 2 (2026-07-20) folded in a review pass that overturned three positions
from revision 1. Revision 3 (2026-07-21) adds the overlay plet, the controls plet
(the default plugin UI), record keying, plet settings persistence, the additive
invariant, and a prior-art survey — and overturned two more positions. Revision 4
(2026-07-21) captures the pluginlet-as-package developer experience (see
"Developer experience"), reflecting landed code: the controls native+webui halves,
controls_demo (verified live), stepper/range/file/color widgets + semantic types,
and the `xi_use_pluginlet` / `plugin.json "pluginlets"` build wiring. See the
**Corrections log**. Revision 5 (2026-07-21) inverts the frontend layering — a plet
OWNS its UI half (source), the app depends on the plet — and records the webui build
consumer; see "Frontend layering". Items still marked DESIGN ONLY below are unbuilt.

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
toolbox/pluginlets/live-view/
├── pluginlet.json          the INDEX: halves / build / requires / version / contract
├── live_view.hpp           native: xi::pluginlet::LiveView (the Derived cell), #include'd into the host plugin DLL
├── contract.ts             SHARED: channel naming, viewport message schema, control schema
└── ui/                     the UI half (SOURCE — the webui build compiles it)
    ├── live-view.ui.ts     mount(el, channel) → renders frames, emits viewport back
    ├── widgets/            xi-image-viewer.svelte, xi-image-editor.svelte
    └── lib/                viewport.mjs, tools.mjs (+ their tests)
```

Author-facing surface is two lines of C++ plus one manifest key:

```cpp
#include <live-view/live_view.hpp>
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
- **the overlay vocabulary lives in a PLET's contract, NOT in core** (point / line /
  polyline / circle / arc / rect / text + verdict styling). Revision 2 said "defined
  once in core" — corrected: the vocabulary *evolves* (new primitive types keep
  arriving), and pinning an evolving thing into the frozen ABI means an ABI change
  per primitive. As a plet contract, a new primitive is a plet version bump and the
  ABI never moves — consistent with the whole "zero core change" thesis. The overlay
  system is itself a plet (the most widely-used one), which is the model eating its
  own dog food. See **The overlay plet** and corrections #4.
- **transport stays byte-blind** — expose forwards, never interprets.

### Record keying: `report` flat, everything else `plet/<fqname>/<var>`

The split above generalizes to the whole pack/record, and the key IS the namespace
(CT). One flat, contracted key holds serious data; every plet-owned var is prefixed
by its fully-qualified plet name, which triples as identity, in-pack address, and
the UI component's binding address (one string, three roles):

```
record entry {
  "report":                       { …contracted inspection truth, ANY shape… }
  "frame":                        <xi/image blob>            ← data plane, shared
  "plet/acme.vision.overlay/$v":   1
  "plet/acme.vision.overlay/draw": [ { target:"frame", prims:[…] } ]
  "plet/acme.vision.overlay/mask": <small inline msgpack bin>   ← embedded copy
  "plet/acme.vision.overlay/preview_ref": "plet/acme.vision.overlay/preview"
  "plet/acme.vision.overlay/preview": <pool-blob entry>        ← zero-copy, ref'd
}
```

- **the line for `report`:** strip every `plet/*` key — can this record still drive
  judgment + SPC? If yes, it was split correctly. `report` = contracted, downstream-
  consumed truth; `plet/*` = auxiliary/presentation/diagnostic, removable with no
  effect on any verdict.
- **structured data nests under the filter key** (a UI component filters one key and
  gets its whole bag); **binary chooses by copy cost** — small/per-frame bin embeds
  in the msgpack (`bin` type, one copy per hop); image-sized or shared data is minted
  as a **pool blob** (a pack-level entry, refcounted, zero-copy) and *referenced* from
  inside the object (`*_ref`). A nested map cannot hold a pool blob, so ref-to-sibling
  dissolves the flat-vs-nested dilemma. Never re-add a per-frame image-sized memcpy
  the system worked to remove (ws zero-copy, expose shared `frame_bytes`).
- **`$v` per plet** (records outlive plet versions; the plet owns its own schema
  version, like mock_camera's schema check). Prefer a `$v` sibling key over baking a
  version into the name string.
- **naming style is OPEN** — dotted FQ (`plet/acme.vision.overlay`, aligning with the
  capability plane's `xi.jpeg.encode`) vs the channel style (`ui/<instance>`, slashed).
  A plet is more capability-like than channel-like, which argues dotted, but this is
  unsettled. Whatever is chosen bakes into records, so a company/toolbox namespace
  layer is cheap insurance now vs. invalidating every stored record later.

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

## The overlay plet

Overlay is not a core feature — it is a plet (per the correction above). Its two
halves:

- **native**: a typed builder that fills declarative draw primitives into the pack,
  with the demand gate HIDDEN inside it — the author writes draw calls
  unconditionally and each is an early-return when nobody is watching (no buffer
  even allocated). The fiddly "is anyone looking" logic is exactly what the plet
  hides, the same gate as `xi::Derived`.
  ```cpp
  xi::pluginlet::Overlay ov{out, "frame"};   // bound to a pack image
  ov.line(p0, p1, Verdict::OK).circle(c, r).text(pos, "12.34mm");
  ```
- **UI**: the generic renderer draws the standard vocabulary; a plugin with a
  special primitive registers a custom **draw hook** instead of forking the renderer
  — which inserts a useful tier 1.5 (standard vocabulary + one custom hook) between
  "no UI code" and "full custom widget", smoothing the escalation path.

Live-view and the overlay plet do NOT talk to each other (the peer-to-peer ban): the
HOST builds the UI pack, hands it to live-view to fill the image and to the overlay
plet to fill the overlay, then pushes it. Host-mediated, no plet-to-plet edge.

## The controls plet — the DEFAULT plugin UI (tier-1's main use case)

The highest-leverage plet is not `live-view` (streaming is the fancy ~20% case) —
it is the **control panel** almost every plugin wants: expose the def parameters as
typed widgets (buttons, sliders, numeric entry with a touch NUMPAD, toggles,
dropdowns) plus read-only I/O readouts. Reframed: **the controls plet is the default
UI every plugin gets; live-view / overlay are opt-in upgrades most plugins never
need.** It is also SIMPLER than live-view (no encode, no viewport, no zero-copy
traps).

### It rides existing paths — zero new transport, zero ABI

The UI needs three operations, each already routed to instances:

| op | for | existing path |
|---|---|---|
| **describe** | fetch the schema to build widgets | `get_def()`'s `$schema` (or `exchange("describe")`) |
| **read** | current values → widgets + readouts | `get_def()` |
| **write / invoke** | change a param (persists) / press a button (transient) | `set_def()` / `exchange()` |

### Native half: `xi::pluginlet::Controls` — replaces def boilerplate

Its whole value is REPLACING the hand-written `get_def`/`set_def` marshalling every
plugin repeats today (mock_camera's is the exact boilerplate). Declare params once;
get get_def / set_def / UI schema / thread-safe access for free:

```cpp
ctl_.tab("Capture")
      .section("Basic").slider("fps",30,1,60).numpad("gain",1.0,0.1,4.0)
      .section("Advanced").collapsed().enumsel("mode","fast",{"fast","accurate"})
    .tab("Output").readout("last_result","Last measured");
// host delegates:
std::string get_def() const override { return ctl_.get_def(); }
bool set_def(const std::string& j) override { return ctl_.set_def(j); }
void process(...) override { auto s = ctl_.snapshot(); int fps = s.i("fps"); … }
```

Three things it centralizes for ALL plugins: (1) **validation declared once** — the
descriptor's min/max/enum; `set_def` MUST clamp/reject against it (never trust the
client), the UI mirrors it. (2) **thread safety** — the set_def-vs-process race
(set_def arrives on any thread; process runs on a dispatch thread) is handled by a
`shared_mutex` + a per-frame `snapshot()`, once, for everyone. (3) **`$v`** so old
persisted defs still parse.

### The UI declaration is a TREE, from a static file OR native

Layout (tabs / collapsible sections) is far easier as a tree than a flat list, so
the schema is a tree of **container** nodes (tabs / section / row / grid) and **leaf**
control nodes. Two sources, one vocabulary, one renderer:

- **static file (the default)** — the whole tree in `plugin.json`; 80% of plugins,
  **zero native code**. This is the "default supports static declarative" baseline.
- **native `get_def` (opt-in)** — native emits the tree, so runtime-dynamic tabs /
  conditional sections / state-driven collapse are just "build the tree." (Blender's
  proven `bpy.props` + `UILayout` model, across a JSON boundary.)

**Separate structure from values or native-emitted trees churn the UI.** The schema
carries `$rev`; the renderer caches the tree and, when `$rev` is unchanged, only
patches new values into existing widgets (keeping focus / a half-dragged slider /
scroll). Native bumps `$rev` only when the STRUCTURE changes. Same dedup discipline
as `Demand::window` / the overlay `report`-vs-data split.

### Three states, three owners (the answer to "where does layout state live")

| state | example | owner | persisted |
|---|---|---|---|
| **values (config)** | fps, gain, mode | `set_def` → instance def | ✅ instance.json |
| **structure (declaration)** | which tabs/sections/controls exist | manifest OR native `get_def` | ❌ it is a declaration |
| **view state** | which tab is open, which section collapsed, scroll | **browser-local, per-operator** | ❌ NOT in def |

The load-bearing rule: **collapse / active-tab is view state, not config.** Native
declares the DEFAULT (`collapsed:true` = start collapsed); the operator expanding it
is their view, and must not round-trip to native or dirty the machine config (which
would re-persist and could touch inspection state). Conditional visibility: a static
`visibleWhen` predicate for simple cases, native omission of the node for arbitrary
logic.

The widget vocabulary is the controls plet's **contract, not core ABI** (same reason
as overlay: widget types evolve). Touch NUMPAD is a first-class, host-owned
input-method surface (Qt Virtual Keyboard's model) — a widget declares "numeric",
the host presents the numpad; the keypad is not baked into each widget.

## Plet settings persistence: a namespace inside the host's def

A plet needs no config-file identity of its own — it borrows the host's def exactly
as it borrows the host's identity and lifecycle. The plet exposes plain (non-ABI)
`get_def()`/`set_def()`; the host DELEGATES a namespaced slice:

```cpp
std::string get_def() const override {
    return xi::Json::object().set("width", w_.load())
        .set("plet/acme.vision.overlay", ov_.get_def()).dump();   // delegate
}
bool set_def(const std::string& j) override {
    auto p = xi::Json::parse(j);
    /* host's own keys… */
    auto s = p["plet/acme.vision.overlay"]; if (s.valid()) ov_.set_def(s);
    return true;
}
```

One name (`plet/<fqname>`) addresses the SAME plet on both the data plane (record)
and the config plane (def) — a UI component knowing its name knows where to read
data AND read/write settings. Notes: config nests plainly (no pool-blob problem, so
no ref-to-sibling needed — that asymmetry with the pack is deliberate); the plet
stamps its own `$v`; set_def/process threading and absent-section tolerance are the
same rules as above (an old instance.json lacking the plet's slice must default
cleanly, not fail). **No new persistence mechanism** — the host def already persists;
the plet just occupies a named slice of it.

## The additive invariant — what touches what (CT)

The whole model is designed so that everything here is **pure-additive and rides
above the frozen ABI** — zero `xi_core` / C-ABI change. Precisely:

- **native halves** (`xi::Derived`, `LiveView`, `Overlay`, `Controls`) — SDK headers
  a plugin `#include`s. Additive, no core, no other-plugin change.
- **UI halves** — webui modules bundled at build time. Additive, no core.
- **data-shape conventions** (`report` / `plet/<fqname>/<var>` keys, overlay `target`,
  ref-to-sibling blobs) — the plugin writes these through the EXISTING pack builder;
  the record already stores arbitrary keyed entries + blobs (spec 30). No format
  change, no core change — a convention each plet honors.
- **config** — a named slice of the host's existing def. No new mechanism.

**The one shared piece that is NOT per-plet** — the upstream-control relay in
**expose** (a plugin, not `xi_core`) — **is now generic, so the invariant holds
fully.** It was hard-coded to `viewport`, which meant every new plet wanting
upstream control had to edit expose. It is now a byte-blind per-channel control
store: `{command:"control", channel, key, value}` writes any key, and the
`xi.ui.sink` probe reply hands back every control for that channel as its own
entry. `viewport` is simply one key (the `{command:"viewport", x,y,w,h}` sugar
still works, so live-view's native half is unchanged). Controls are stored only
while the channel is SUBSCRIBED and dropped on unsubscribe, so the store is
bounded by `subscribed_` with no separate cap.

So the statement is now unqualified: **zero frozen-ABI / zero xi_core change, and
a new pluginlet — including one that needs upstream control — is purely additive.**

## Prior art (control-panel frameworks surveyed) — what to adopt

A 2026-07 survey of mature declarative/schema-driven control-panel libraries, to
ground the controls-plet API rather than invent it. Full findings off-doc; the
load-bearing conclusions:

| library | model | what we take |
|---|---|---|
| **Foxglove Studio** settings | panel emits a typed settings tree; host renders generically; changes via `actionHandler` | the closest production analogue to our exact model (same domain family) — validates the architecture |
| **Blender** `bpy.props`+`UILayout` | typed prop declaration (min/max/**subtype**/soft-range) ⟂ separate draw-tree | structure⟂values split; **subtype** hint decides units + which touch editor; **soft vs hard range** for slider+numpad |
| **JSON Forms** | JSON Schema + UI Schema (Categorization = tabs) + **tester-ranked renderer registry** | tester-ranked widget registry (third parties add widgets without editing a central map) > flat name map |
| **Tweakpane v4** | `addBinding`/`addBlade({view,…})`, folders/tabs, `readonly`+`interval` monitors, `createPlugin` | blade/plugin layer as the widget IMPLEMENTATION tier; monitor = readonly binding + poll interval |
| **Leva** | schema-object `useControls`, `createPlugin({normalize,sanitize,format})` | per-widget **normalize/sanitize/format** — `sanitize` IS our native clamp mirror; `format` gives readout strings |
| **RJSF** | JSON Schema + uiSchema + widgets registry | schema ⟂ uiSchema ⟂ code-registry triad (referenced by string) |
| **lil-gui / dat.GUI** | infer widget from value type | inference as a convenience layer only — its object-mutation binding assumes shared memory, which our get_def/set_def boundary breaks, so bindings must be **controlled**, not mutate-in-place |
| **Dear ImGui** | immediate mode | cautionary: great native ergonomics but the UI is per-frame code — cannot serialize / ship a manifest / persist. We are declarative/retained on purpose |
| **ControlP5** | imperative `addSlider/Numberbox/Toggle`, Accordion/Tab | C++ builder naming reference (LGPL — design only) |

Landing route if built: **web half** = JSON-Forms-style schema/uiSchema/tester-registry
skeleton + Tweakpane blades as the widget layer + a custom touch numpad; **native half**
= Blender-style declare(+subtype/soft-range)/layout-tree, builder naming after
ControlP5. Licenses of the borrow-from set are MIT/Apache (Tweakpane, Leva, JSON
Forms, RJSF); ControlP5 LGPL and Blender GPL are design references only.

## Developer experience: the pluginlet as a package

A pluginlet is, to a plugin author, **a fixed-structure folder you pull into your
plugin build** — and the same folder feeds the webui and the runtime. The design
goal is that **90% of plugins never write UI code**: they declare params and get a
consistent panel, and consistency is the free byproduct of everyone going through
the same constrained path ("declare, don't draw").

### A plet is a folder + a manifest (the manifest is the index)

```
toolbox/pluginlets/<name>/
├── pluginlet.json      the INDEX: halves / build / requires / version / contract
├── <name>.hpp          native half (header-only, or + .cpp)
├── <name>.ui.ts|.mjs   webui half
├── contract.ts         the shared contract (single source of truth)
├── assets/  (opt)      icons, or the .wasm for a logic-bridge plet
└── test/    (opt)      plet-local tests
```

`pluginlet.json` is not documentation — tooling reads it. It carries `halves`
(where each half's entry is), `build.native` (link deps), `requires` (capabilities/
planes/host methods), `version`, and `contract`. The folder layout can be flat or
nested; the manifest is what tooling resolves against.

### One declaration, three consumers

A plugin opts into a plet with ONE line in its `plugin.json`:

```json
"pluginlets": ["controls"]
```

That single declaration drives three separate consumers — the "one name, three
roles" pattern again (identity / build address / bind address):

| consumer | what it does with `"pluginlets"` | status |
|---|---|---|
| **C++ build** | `xi_wire_pluginlets(target dir)` reads it → `xi_use_pluginlet(target <name>)` per entry → applies the plet's include root + `build.native.links` from its manifest | **landed** |
| **webui build** | the `xi-pluginlet-ui` vite plugin scans each manifest's `build.ui.widgets` and compiles/bundles them | **landed** |
| **runtime** | the backend surfaces `pluginlets` on `list_plugins`; `mountPluginlets(host,{client,instance,pluginlets})` mounts each plet's UI from a manifest-generated registry | **landed** |

The C++ consumer is landed: `xi_use_pluginlet` / `xi_wire_pluginlets`
(toolbox/CMakeLists.txt) turn "declare the plet in plugin.json" into the only step —
no per-plugin CMake edit, no per-plugin knowledge of the plet's deps. controls_demo
builds purely from its `"pluginlets":["controls"]`.

### The DX surface (what exists, what's missing)

| DX aspect | state |
|---|---|
| **① one-step include** | ✅ `xi_use_pluginlet` + `plugin.json` `"pluginlets"` → `xi_wire_pluginlets` |
| **② dependency validation** | ⚠️ manifest has `requires`; nothing checks it (warn if live-view used without expose) |
| **③ versioning** | ⚠️ manifest `version` + runtime `$v`/`$rev`; no plugin-side pin/compat check |
| **④ scaffold** | ❌ `xinsp2-plugin` skill should generate a Controls-based plugin (UI out of the box); a `new-pluginlet` generator |
| **⑤ authoring loop** | ⚠️ `recompile_project_plugin` hot-reload + `$rev` structural re-render exist; no edit→rebuild→re-render watch |
| **⑥ plet-local tests** | ⚠️ tests exist (test_controls, schema-panel.node) but live in backend/ui-components, not the plet's `test/` |
| **⑦ vendoring** | ✅-ish by construction (self-contained folder + declaration); custom-element widgets + vanilla renderer make the UI half host-agnostic |
| **⑧ discoverability** | ⚠️ `contract.ts` is the source of truth; no cheatsheet / `list-pluginlets` index |

### The "90% kit" — consistency by construction

The controls plet is the opinionated default UI kit. Consistency comes from three
shared, non-optional things, not from a style guide:

- **one renderer** (`mountSchema`) + **one theme** (vscode-theme.css) → identical
  look, auto light/dark, touch-ready, no per-plugin CSS;
- **a fixed widget vocabulary** (contract.ts) — no raw HTML on the happy path, so a
  plugin *can't* drift;
- **semantic types** (`.sem("threshold"/"gain"/"roi"/…)`, orthogonal to the widget)
  → the same semantic renders the same units/format/touch-editor everywhere.

Coverage aims at 90%-no-custom-widget: slider / numpad / stepper / range / toggle /
dropdown / radio / text / file / color value controls, button / readout / view,
title / label / divider, in tabs / grid / sections with span/rows/caption. The 10%
that need more escape to a custom widget (a `.svelte` custom element) or a vanilla
panel — a deliberately separate tier that does not dilute the default.

**Framework note:** the widgets are Svelte 5 compiled to standard **custom
elements** (`<xi-slider>` …); the renderer/orchestration layer (`mountSchema`,
ws-client) is **vanilla ES modules**. So Svelte is an implementation detail of the
widgets — a plet's UI half is host-agnostic and portable (any HTML/VS-Code-webview/
React host uses `<xi-*>` without adopting Svelte). This suits "plet = distributable
folder" better than committing the whole UI to one framework.

## Frontend layering: a plet OWNS its UI, the app depends on the plet

Revision 4 still had the UI backwards: `mountSchema` lived in the app library
(`@xinsp2/components`) and the plet borrowed it. That makes the app a shared hub
everything couples to. **Inverted (CT): a pluginlet is a LEAF that owns its own UI;
the app layer — extension / HMI / core — DEPENDS ON the plet when it wants that
UI, never the reverse.** This is the same principle the codebase already applies to
plugins (`expose` owns its UI; core is a plugin hub), extended to plets.

```
                design tokens / theme CSS          ← the real consistency contract
                          ▲
   controls plet                      live-view plet          ← LEAVES; own their UI
   ui/mount-schema.mjs                ui/live-view.ui.ts
   ui/widgets/xi-{slider,number,      ui/widgets/xi-image-{viewer,editor}
     toggle,radio,dropdown,text}      ui/lib/{viewport,tools}.mjs
   ui/lib/options.mjs
                          ▲
   @xinsp2/components (app): dashboard cards, layout engine, XiClient,
   vscode-shim, webapp export, xi-button/badge/trace    ← extension + HMI
```

**Plets are SOURCE, both halves.** Like the native `.hpp`, the UI half ships as
source and the CONSUMER'S build compiles it (native → the plugin's CMake via
`xi_use_pluginlet`; UI → the webui's vite/Svelte build). A plet never ships a
prebuilt artifact. The honest consequence, symmetric on both sides: vendoring a
plet requires the consumer to have the toolchain — a C++ compiler for the native
half, a Svelte/vite build for the UI half.

**The webui build is the third consumer of the manifest.** `xi-pluginlet-ui` (a
vite plugin in `ui-components/vite.config.js`) scans every
`toolbox/pluginlets/*/pluginlet.json` for `build.ui.widgets` and exposes them as
one virtual module, so `src/index.js` just imports `virtual:xi-pluginlet-ui`.
Adding or removing a plet widget needs no edit in the app — exactly mirroring
`xi_use_pluginlet` on the native side. That closes two of the three consumers from
"One declaration, three consumers"; only the runtime auto-mount remains.

**A plet must not import upward — and the build enforces it.** Moving the widgets
proved this concretely: `xi-image-editor` imported `../lib/viewport.mjs` from the
app layer and the build failed loudly. Each shared lib turned out to be used by
exactly one plet's widgets, so `options.mjs` went to controls and
`viewport.mjs`/`tools.mjs` to live-view. A plet's upward import is now a build
error, not a silent coupling.

**Consistency does NOT come from sharing component code** — that would be
consistency by coincidence, and it dies the moment anything is reimplemented. It
comes from three things that survive independent implementations:

1. **design tokens / theme CSS** (`vscode-theme.css`) — spacing, colour, dark/light;
2. **the declarative contract** — `$schema` + the widget vocabulary (`contract.ts`);
3. **semantic types** (`sem`) — the same semantic renders with the same units and
   the same touch editor everywhere, regardless of which element draws it.

**Widget authoring stays Svelte, and that is not a leak.** Each widget compiles to
a standard custom element (`<svelte:options customElement>`), so consumers use
`<xi-slider>` with no framework of their own; Svelte is an implementation detail
contained by the build. A plet's UI half is therefore host-agnostic and portable,
which is what makes "plet = distributable folder" viable.

**What stays in the app layer:** anything not tied to a plet — dashboard cards, the
layout engine, `XiClient`, the vscode shim, webapp export, and the widgets no plet
owns (`xi-button`, `xi-badge`, `xi-trace`). The app also keeps the *chooser*
(`mountInstancePanel`: `$schema` → the plet renderer, else the flat panel) — a plet
never falls back, because picking a renderer is not a plet's business.

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
4. **"the overlay vocabulary is defined once in CORE"** (rev 2) — the vocabulary
   evolves (new primitive types keep arriving); pinning it in the frozen ABI means an
   ABI change per primitive. *Overturned by:* the same "zero core change" thesis this
   doc rests on. Corrected: overlay is itself a **plet**; the vocabulary is its
   contract, a new primitive is a plet version bump.
6. **"the UI half is a module in the app library that the plet points at"** (rev 2-4)
   — backwards: it made `@xinsp2/components` a hub every plet coupled to, and the
   plet folder was not self-contained. *Overturned by:* CT — a plet is a LEAF that
   owns its UI; the app (extension/HMI/core) depends on the plet. Corrected in code:
   mountSchema + the form widgets + their libs now live in the plets, discovered by
   the build from each manifest. Consistency comes from tokens + `$schema` + `sem`,
   not from sharing component code.
5. **"live-view is the first/reference pluginlet"** (rev 1 framing) — streaming is the
   fancy ~20% case. *Reframed:* the **controls plet** (schema-driven config panel) is
   the default UI ~80%+ of plugins actually need and is simpler; if anything is built
   next, it is the higher-leverage one.

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
  `toolbox/pluginlets/live-view/` two-halves + contract + manifest; UI widget
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

- **the `report` / `plet/<fqname>/<var>` record keying** (filter keys, ref-to-sibling
  blobs, per-plet `$v`) — agreed shape, not implemented; nothing in the pack contract
  changed. Naming style (dotted vs slashed) is an OPEN question.
- **the overlay plet** — vocabulary (point/line/…/text + verdict styling), the native
  `Overlay` builder, and the generic webui renderer + custom draw hook: none written.
- **the controls plet — LANDED (native + webui renderer + a live plugin).** Native
  `xi::pluginlet::Controls` (`toolbox/pluginlets/controls/controls.hpp` + `contract.ts` +
  manifest; `test_controls`, 14 tests). Webui `mountSchema`
  (`toolbox/pluginlets/controls/ui/mount-schema.mjs` — OWNED BY THE PLET, rev 5)
  renders the `$schema` tree with the real built xi-* Svelte custom elements, wired
  to set_instance_def (`ui/mount-schema.test.mjs`, 6 tests + a Playwright render of
  both tabs). The plet also owns its widgets (`ui/widgets/xi-{slider,number,toggle,
  radio,dropdown,text}.svelte`) and `ui/lib/options.mjs`. `controls_demo` (a real plugin) proves
  get_def→`$schema` / set_def-validation / readouts live in the running backend, and
  builds purely from its `plugin.json "pluginlets"`. Widgets: slider/numpad/stepper/
  range/toggle/dropdown/radio/text/file/color + button/readout/view + title/label/
  divider, with semantic types (`sem`). The dedicated **xi-stepper / xi-range /
  xi-color / xi-file** elements and the **host-owned touch numpad**
  (`ui/lib/numpad.mjs`, one surface per page — Qt-Virtual-Keyboard model) are now
  LANDED too, so nothing degrades.

  **The widget REGISTRY (rev 6, LANDED)** — CT's direction: zero-code is not the
  goal; a clear, small API with one explicit line of wiring beats magic. The
  controls plet is an **extensible widget vocabulary**, not a closed set:
  `registerWidget(name, impl)` / `unregisterWidget(name)` (owned by
  `mount-schema.mjs`, re-exported by `@xinsp2/components`) lets the consuming
  webui add a widget name or override a built-in. An impl is a tag string (a
  bound value control, e.g. a chart plet's `"xi-chart"`) or a factory
  `(node, ctx) => el | {el, update?(state), destroy?()}` with
  `ctx = {doc, client, instance, state, pushDef}`; `update` joins the panel's
  `refresh()`, `destroy` its `destroy()`. Per-mount overrides:
  `mountSchema(host, {widgets: {name: impl}})` beats the global registry. This is
  how OTHER plets plug into the controls layout without controls knowing them —
  the canonical case being **live-view claiming `view`**:

      registerWidget("view", (node, { instance }) => {
        const el = document.createElement("div");
        const dispose = mountLiveView(el, node.channel || `ui/${instance}`, transport);
        return { el, destroy: dispose };
      });

  (demoed with a synthetic frame source in `ui-components/demo/schema.html`;
  channel defaults to `ui/<instance>` when the schema omits it, softening the
  two-places-name-the-channel seam). Tested in `ui/mount-schema.test.mjs`
  (factory + view override + tag-string + per-mount override + lifecycle).
  Unknown widget (neither built-in nor registered) → an INFO placeholder in the
  layout slot naming the widget, the orphaned key, and the exact
  `registerWidget("…")` line missing — never a silently guessed control.
  FOOTGUN (hit for real): the registry is MODULE-SCOPED, so a bundle-consuming
  app must take `mountSchema` and `registerWidget` from the SAME bundle; mixing
  the bundled copy with a direct source import gives two registries and the
  registration lands in the one mountSchema doesn't read (the placeholder is
  what surfaces it). App-custom widgets in Svelte: the reference is
  `ui-components/src/demo/TeachPanel.svelte` + `register-teach-panel.svelte.js` —
  a PLAIN (non-CE) Svelte component tree mounted by the factory via svelte
  `mount()/unmount()`, a `$state` object bridging the panel's `refresh()` into
  runes, child components composed normally, and the plet's xi-* custom elements
  mixed in as plain tags.

  **The NATIVE side of the registry** (same rev): the builder does not grow a
  method per custom widget — it has two orthogonal escape hatches.
  `.comp(widget, key?)` declares a leaf whose widget name controls does not
  know: keyed = a plugin-pushed data slot (readout semantics — `set_readout(key,
  json)` feeds it, set_def never writes it), keyless = presentation/stream-only,
  paired with the `.channel(ch)` modifier. `.as(widget)` re-skins a typed INPUT
  builder (`.slider("pressure",0,0,10).as("gauge")`) keeping its value contract —
  clamping and min/max ride the schema unchanged, only presentation swaps.
  Multiple graphs of one component = multiple `.comp("chart", key)` nodes, each
  bound to its own key/channel (registration is per widget TYPE; instantiation is
  per schema NODE). `test_controls` covers it (custom_components).
  STILL DESIGN ONLY within the controls plet: wiring `sem` to
  units/format/touch-editor selection (it is carried, not yet acted on); the
  `view`↔live-view registration against a REAL backend transport (the registry
  mechanism and the demo wiring exist; no e2e); native dynamic-tree
  `$rev` bumping (the tree is static today, `$rev` constant).
- **plet settings persistence** — the delegated `plet/<fqname>` def slice: convention
  only, no helper written.
- **the thin `Plet` interface** (`publish()` + `stats()` for uniform lifecycle /
  metrics reporting under the host's owner name) — not written; plain members remain
  the recommendation until several pluginlets exist.
- **prior-art landing route** (JSON-Forms skeleton + Tweakpane blades + Blender-style
  native declare) — a survey conclusion, not a commitment.

The one known gap in what IS built: `ui/live-view.ui.ts` infers full-image
dimensions from the first frame, but the native half may already have downsampled it
(`max_edge`), so the widget's viewport coordinate basis can be the downsampled size
rather than true full-image pixels. Fixing it means the producer reporting true
full-image `w,h` alongside the frame. live-view's UI half is still an untested TS
sketch (its widgets + viewport/tools libs now live in the plet and DO build); only
its native half and the expose relay are compiled and tested.
