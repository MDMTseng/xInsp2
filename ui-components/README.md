# xi-components — xInsp2 UI web components

Svelte-authored UI components that **compile to standard custom elements**, plus a
shared **WS-client shim**. The Svelte build step is contained to *this* folder —
every consumer (the HMI's `.mjs`, plugin `ui/index.html`, VS Code webviews, an
external webapp) stays framework-free and just uses `<xi-*>` tags.

Design + rationale: [`../docs/roadmap/webui-and-ui-export.md`](../docs/roadmap/webui-and-ui-export.md)
(tasks #71–#80).

## Layout

```
src/
  components/        Svelte components, each a custom element (<svelte:options customElement=…/>)
    xi-slider / xi-number / xi-toggle / xi-radio / xi-dropdown   key-bound controls (value prop + change/input events)
    xi-trace.svelte          watch an output key across runs (update(items); latest/history; sparkline)
    xi-image-viewer.svelte   canvas viewer: wheel-zoom/pan/pixel-probe; tap-out setFrame/fit + pixelpick/viewchange
    xi-image-editor.svelte   teach editor: draw point/rect/polygon (pull model); tap-out setFrame/setTool + commit/cancel
  dashboard/         the HMI dashboard, importable (one source, used by hmi/ + external apps)
    cards.mjs        xi-card-* dashboard cards (verdict/yield/throughput/groups — run_result/compute_ms/dispatch_stats)
    layout.mjs       N-ary split/tabs layout engine (pure)
    dashboard.mjs    mountDashboard(host,{client,dashboard}) — render cards from a config + feed from WS
  lib/
    viewport.mjs     pure pan/zoom math (unit-tested), generalizes imageViewerPanel.ts
    tools.mjs        teach-tool state machines (point/rect/polygon) + registerTool
    options.mjs      normalize an options array / JSON-string attribute for radio/dropdown
  ws-client.mjs      XiClient — generic transport: connect + version check + cmd/exchange +
                     event/log/instances subscriptions + onBinary(raw bytes) passthrough
  auto-panel.mjs     mountPanel(host,{client,instance,descriptor?}) — descriptor → wired
                     widget sections; inferDescriptor(def) for a zero-config panel
  index.js           registers all xi-* elements; re-exports XiClient + mountPanel + dashboard
demo/
  index.html         drop-in usage of <xi-slider> (vanilla)
  poc.html           end-to-end PoC: viewer + slider wired to a live backend over WS
test/                node tests (viewport invariants + jsdom element smoke + live-backend WS smoke)
dist/                build output (git-ignored)
```

`npm test` runs 8 checks: the viewport pan/zoom invariants (deterministic), the
built `<xi-slider>` jsdom smoke, and the WS-shim round-trip against a real backend.
Open `demo/poc.html` against a running backend to exercise the browser-interactive
parts (canvas render, drag/zoom, pixel probe, live preview) by hand.

## Build

```
npm install
npm run build      # → dist/xi-components.esm.js (ESM) + dist/xi-components.js (UMD drop-in)
npm test           # jsdom element smoke + WS-shim smoke vs a real backend
```

## Two consumption models (same library)

1. **Drop-in** — load the built bundle, use the tags in plain HTML:
   ```html
   <script type="module" src="xi-components.esm.js"></script>
   <xi-slider label="Threshold" min="0" max="255" value="128"></xi-slider>
   ```
2. **Library-import** — an external webapp imports the package + the WS shim:
   ```js
   import "@xinsp2/components";                 // registers <xi-*>
   import { XiClient } from "@xinsp2/components/ws-client";
   // checkVersion:true fails fast on a mismatched backend (canonical XI_VERSION_RE);
   // retry rides out a not-yet-up / single-client-busy backend on the first connect.
   const c = await new XiClient("ws://127.0.0.1:7823/")
     .connect({ checkVersion: true, retry: { attempts: 20, delayMs: 250 } });
   ```

Components expose a **tap-out** API (properties / methods / `change`·`input`
events) the host calls directly — not an iframe postMessage channel. The
components stay decoupled from the WS layer.

## Usable UI for free — `mountPanel`

A plugin declares a **control-descriptor** in its `plugin.json` manifest; the
renderer turns it into wired widget sections — no UI code:

```js
import { XiClient, mountPanel } from "@xinsp2/components";
const client = await new XiClient(url).connect();
await mountPanel(document.getElementById("panel"), {
  client, instance: "bin0",
  descriptor: [{ section: "Threshold", tag: "control", controls: [
    { type: "slider", key: "threshold", label: "Threshold", min: 0, max: 255 },
    { type: "dropdown", key: "mode", options: ["light", "dark"] },
  ]}],
});
```

Editing a control reads the current def, patches the key, and sends the **full**
def back via `set_instance_def` (no key loss regardless of plugin merge
semantics). Omit `descriptor` and it's **inferred** from the instance's def
(number→number, boolean→toggle) — a basic panel with zero declaration. Sections
carry a `tag` (`setup`/`control`/`status`) — the unit of UI export below.

## Manifest param-panel — `mountParamPanel` (single-parameter `set_param`)

Where `mountPanel` sends the **whole def** (`set_instance_def`), a plugin manifest
`params` block drives the **single-parameter** `set_param` path (doc 31):

```js
import { XiClient, mountParamPanel } from "@xinsp2/components";
mountParamPanel(document.getElementById("params"), {
  client,
  params: [                                   // the manifest params block
    { name: "sigma", min: 0, max: 10, default: 3 },   // min&max → slider
    { name: "mode", options: ["fast", "fine"] },      // options → dropdown
    { name: "invert", default: false },               // boolean → toggle
  ],
});
```

Each param is `{ name, type?, min?, max?, step?, default?, options?, label? }`;
`type` (`slider`/`number`/`toggle`/`radio`/`dropdown`/`text`) is inferred from
`default`/`options`/`min`&`max` when omitted (`paramsToControls` exposes the
mapping). Editing a control fires `client.setParam(name, value)` — one named
parameter, the plugin validates it (rsp ok / error) — and emits an `xi-param`
`{name,value}` event. The returned handle has `values()` / `setValues(map)` /
`destroy()`.

## Run a plugin's VS Code webview UI in a browser — the `acquireVsCodeApi` shim

A plugin's own `ui/index.html` (the toolbox panels) is written VS Code-webview
style: it calls `acquireVsCodeApi()` and talks `{type:'exchange',cmd}` ↔
`{type:'status',…}` with `data-param` / `data-action` conventions. The **official
shim** (integration_test FR-3) runs it **unchanged** in a plain browser over WS —
no per-app adapter, no panel rewrite:

```js
import { XiClient, installVsCodeShim } from "@xinsp2/components";
const client = await new XiClient(url).connect();

// Mount the vendored panel in an iframe and install the shim on ITS window
// BEFORE the panel script runs (each panel binds to one instance):
const frame = document.querySelector("#meas0-panel");   // <iframe src=".../meas0/ui/index.html">
frame.addEventListener("load", () =>
  installVsCodeShim({ client, instance: "meas0", win: frame.contentWindow }));
```

The shim's `postMessage({type:'exchange', cmd})` → `client.exchange(instance, cmd)`
→ `window.postMessage({type:'status', …reply})` (the plugin's status body spread
at the top level; `type` written last so a body's own `type` never shadows the
envelope). `getState`/`setState` are `sessionStorage`-backed, matching the
webview persistence contract. `createVsCodeApi(opts)` returns the bare `vscode`
object if you install it yourself. **No backend change** — it is pure client-side
transport over the shipped WS `exchange` verb.

## Declarative renderer vocabulary v1 — `renderDescriptor`

The no-code display path (doc 31): a self-describing blob's **descriptor** (its
canonical-msgpack map — `t`, `w`, `h`, `c`, `dt`, plus an optional `render` hint
and per-renderer keys) picks the renderer for its payload. One entry point:

```js
import { renderDescriptor } from "@xinsp2/components";
renderDescriptor(host, { desc, payload /* Uint8Array/ArrayBuffer */, refs });
```

`pickRenderer(desc)` chooses by the explicit `render` hint, else infers from
`t` + `dt` + shape. The descriptor keys each renderer reads:

| renderer | selected when | descriptor keys | payload |
|---|---|---|---|
| `image`   | `t:"xi/image"`, `dt:"u8"` | `w`, `h`, `c` | `w*h*c` bytes → RGBA canvas (gray/RGB/RGBA) |
| `heatmap` | `render:"heatmap"` or `xi/image` `dt` f32/u16/f64 **2-D** | `w`, `h`, `dt`, `range?:[min,max]`, `colormap?:"viridis"\|"gray"\|"jet"` | `w*h` scalars → colormapped canvas |
| `profile` | `render:"profile"` or an f32/u16/f64 **1×N / N×1** | `w`/`h` (one = 1) or `n`, `dt`, `range?`, `width?`, `height?`, `color?` | `N` scalars → polyline chart |
| `overlay` | `render:"overlay"` | `w`, `h`, `shapes:[…]`, `image?:<ref key>`, `width?`, `height?` | (none) shapes over an image `ref` |
| `table`   | `render:"table"` | `value?` (else the payload) | canonical-msgpack **map** → key/value rows |
| `hex`     | fallback (unknown `t`) | `t` | any bytes → `{type,size,preview}` card |

`overlay` shapes are `{type:"point",x,y,r?,color?}`, `{type:"box",x,y,w,h,color?}`,
`{type:"polyline",points:[[x,y],…],color?,closed?}` in descriptor (`w`×`h`) space,
scaled to the viewport. `dt` ∈ `u8`/`u16`/`i32`/`f32`/`f64` (little-endian). Each
renderer splits a **pure compute core** (`imageRGBA`, `heatmapRGBA`,
`profilePoints`, `overlayOps`, `tableRows`, `readScalars`, `normalize`,
`COLORMAPS` — all exported and unit-tested) from a thin canvas/DOM draw, so the
geometry/color math is testable and reusable outside the canvas.

## Import the whole HMI dashboard — `mountDashboard`

An external webapp can drop in the entire composable HMI dashboard, not just
individual widgets:

```js
import { XiClient, mountDashboard } from "@xinsp2/components";
const client = await new XiClient(url).connect();
mountDashboard(document.getElementById("dash"), {
  client,
  dashboard: { title: "Line 1", layout: { dir: "row", weights: [1, 1], children: [
    { card: { type: "verdict", bind: { result: true } } },
    { card: { type: "yield", bind: { result: true } } },
  ] } },
});
```

It renders the `xi-card-*` cards via the layout engine and feeds them from the
generic WS streams (run_result / run_finished / status / dispatch_stats). RUN
mode (read-only); the standalone `hmi/` keeps its own compose editor and sources
the cards + layout from this same library (one source). The cards carry **no**
preview/vars/gid coupling — a plugin's own webUI renders per-frame value/image
tiles from frames it decodes itself.

## UI export — library-import scaffold

`export/create-webapp.mjs` scaffolds an **external** webapp project that owns its
stack and imports the components, with WS pre-wired (version-pinned) and an
example composition (control panel + generic `xi-image-viewer`) you then develop.
The bundle is vendored (runs offline); swap for the `@xinsp2/components` npm
package when you add a build step.
```
node export/create-webapp.mjs my-hmi/ "Line 1 HMI" ws://plc:7823/
```
