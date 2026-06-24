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
    xi-slider.svelte
  ws-client.mjs      XiClient — connect + orchestrator verbs + vars/preview subscribe
  protocol.mjs       pure WS decoders (parseVars / decodePreviewFrame)
  index.js           registers all xi-* elements; re-exports XiClient + protocol
demo/index.html      drop-in usage (loads the built bundle, vanilla)
test/                node tests (jsdom DOM smoke + live-backend WS smoke)
dist/                build output (git-ignored)
```

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
events) the host calls directly — not an iframe postMessage channel. Wiring a
control's `change` → `set_instance_def` is the consumer's / auto-renderer's job
(see task #76); the components stay decoupled from the WS layer.
