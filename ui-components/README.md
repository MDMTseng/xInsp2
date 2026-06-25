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
    cards.mjs        xi-card-* dashboard cards (verdict/value/image/spc/yield/throughput/groups)
    layout.mjs       N-ary split/tabs layout engine (pure)
    dashboard.mjs    mountDashboard(host,{client,dashboard}) — render cards from a config + feed from WS
  lib/
    viewport.mjs     pure pan/zoom math (unit-tested), generalizes imageViewerPanel.ts
    tools.mjs        teach-tool state machines (point/rect/polygon) + registerTool
    options.mjs      normalize an options array / JSON-string attribute for radio/dropdown
  ws-client.mjs      XiClient — connect + orchestrator verbs + vars/preview subscribe
  protocol.mjs       pure WS decoders (parseVars / decodePreviewFrame)
  auto-panel.mjs     mountPanel(host,{client,instance,descriptor?}) — descriptor → wired
                     widget sections; inferDescriptor(def) for a zero-config panel
  index.js           registers all xi-* elements; re-exports XiClient + protocol + mountPanel
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
   const c = await new XiClient("ws://127.0.0.1:7823/").connect({ checkVersion: /\d+\.\d+\.\d+/ });
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

## Import the whole HMI dashboard — `mountDashboard`

An external webapp can drop in the entire composable HMI dashboard, not just
individual widgets:

```js
import { XiClient, mountDashboard } from "@xinsp2/components";
const client = await new XiClient(url).connect();
mountDashboard(document.getElementById("dash"), {
  client,
  dashboard: { title: "Line 1", layout: { dir: "row", weights: [1, 1], children: [
    { card: { type: "value", bind: { var: "fg_pct" } } },
    { card: { type: "image", bind: { var: "result" } } },
  ] } },
});
```

It renders the `xi-card-*` cards via the layout engine and feeds them from the
WS streams (vars / preview / run_result / dispatch_stats). RUN mode (read-only);
the standalone `hmi/` keeps its own compose editor and now sources the cards,
layout, and protocol decoders from this same library (one source).

## UI export — two shapes

- **A — status monitor (generic, auto).** `export/build-monitor.mjs` emits a
  self-contained static folder: a read-only `mountMonitor` wall (value tiles,
  `xi-trace` sparklines, `xi-image-viewer` fed by the preview stream) + the
  vendored bundle. `collectStatusItems(descriptors)` flattens a project's
  `status` sections into the item list.
  ```
  node export/build-monitor.mjs config.json out/    # → out/index.html + bundle
  ```
- **B — full app (bespoke, library-import).** `export/create-webapp.mjs` scaffolds
  an **external** webapp project that owns its stack and imports the components,
  with WS pre-wired (version-pinned) and an example composition (panel + viewer +
  monitor) you then develop. The bundle is vendored (runs offline); swap for the
  `@xinsp2/components` npm package when you add a build step.
  ```
  node export/create-webapp.mjs my-hmi/ "Line 1 HMI" ws://plc:7823/
  ```
