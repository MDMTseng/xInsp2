# Building a plugin UI with xi-components

`ui-components/` ships a small library of framework-agnostic **web components** —
sliders, an image viewer, a teach editor, dashboard cards — that you drop into a
plugin's `ui/index.html` (or any webapp) as plain `<xi-*>` tags. No framework, no
build step for you: the Svelte build is contained to the library; you just load
the bundle and use the tags.

The components are **tap-out**: each exposes plain properties, methods, and DOM
events. You wire those to whatever channel your page has — the VS Code webview
bridge, or a direct WebSocket. That's the whole model.

---

## Play with it first (5 minutes)

The fastest way to understand the components is to run the playground pages
against a live backend:

```bash
# 1. build the library once (one-time; needs node)
cd ui-components && npm install && npm run build

# 2. serve the folder (any static server)
npx --yes serve .            # → http://localhost:3000

# 3. start a backend in another terminal
backend/build/Release/xinsp-backend.exe --port=7823
```

Then open, against that backend:

| Page | What it shows |
|---|---|
| `demo/index.html` | a single `<xi-slider>` — properties + `change`/`input` events |
| `demo/poc.html?ws=ws://127.0.0.1:7823/` | connect, a slider bound to an instance via `set_instance_def`, the image viewer (zoom/pan/pixel-probe) |
| `demo/dashboard.html?ws=ws://127.0.0.1:7823/` | a whole HMI dashboard via `mountDashboard` |

Edit the HTML, refresh, see the change. These are your sandbox.

---

## The components

| Tag | Purpose | Key API (tap-out) |
|---|---|---|
| `xi-slider` / `xi-number` / `xi-toggle` / `xi-radio` / `xi-dropdown` | key-bound controls | `.value` property; emits `input`/`change` (`detail.value`) |
| `xi-image-viewer` | image display: wheel-zoom / pan / pixel-probe | `.setFrame(src)` / `.fit()`; emits `pixelpick` `{x,y,rgb}`, `viewchange` |
| `xi-image-editor` | teach editor: draw point / rect / polygon | `.setFrame(src)` / `.setTool(id)`; emits `commit` `{tool,result}`, `cancel` |
| `xi-trace` | watch an output key across runs | `.update(items)`; `.latest` / `.history` |
| `xi-card-*` | dashboard cards (value/image/verdict/spc/…) | fed by `mountDashboard` |

Higher-level helpers (also exported): `mountPanel` (auto-UI from a descriptor),
`mountMonitor` (status wall), `mountDashboard` (the whole dashboard), `XiClient`
(the WS shim).

---

## Wiring to the backend — pick your context

> **Core image preview removed.** The
> `subscribe`/`unsubscribe` commands and the binary image-preview frames that the
> `onPreview` / `subscribeImage` flow below relied on have been **removed** from
> the backend. Control wiring (params via `set_instance_def` / `exchange_instance`)
> and `status` are unaffected and remain the live path. **Output surfacing for a
> plugin UI now goes through the shipped `expose` plugin** (`plugins/expose`,
> `sink:true`): its live contract is the `subscribe` / `unsubscribe` / `get` /
> `list_channels` exchange commands plus one atomic `XEX1` binary frame per
> channel pushed via the ABI v8 `emit_binary` host call. See
> [`write-a-script.md`](write-a-script.md),
> [`../reference/ws-protocol.md`](../reference/ws-protocol.md) (*The `expose`
> plugin*), and [`../reference/c-abi.md`](../reference/c-abi.md) (`emit_binary` v8 /
> `compress_image` v9). The `preview` message + `subscribeImage` examples below are
> kept as the prior pattern for reference.

#### The `expose` plugin's consumer contract

A viewer drives the `expose` instance over `exchange_instance` (subscription +
pull) and receives a live binary push. Output is organised by string **channel
id** (created implicitly on first send); a channel is the unit of subscription and
of the UI tab. From `plugins/expose/plugin.json` + `src/expose.cpp`:

| Command | Args | Returns |
|---|---|---|
| `subscribe` | `{ channels: [...] }` | `ok` — start pushing frames for these channels |
| `unsubscribe` | `{ channels: [...] }` | `ok` — stop pushing them |
| `get` | `{ channel }` | `{ found, channel, seq, frame_b64 }` — base64 of the latest `XEX1` frame (pull) |
| `list_channels` | — | channel/tab metadata for the UI tabs |

Per run, for each **subscribed** channel, the plugin pushes one self-contained
`XEX1` binary frame (magic `XEX1` + msgpack `{v, channel, seq, json, images:[{key,
jpeg}]}`) via the host `emit_binary` call (ABI v8). Values ride as a JSON string
(`json`) in record key order; each image is JPEG-compressed in `images[]`. A frame
self-identifies its `channel`, so a client filters broadcast frames to the right
tab with no side-channel. Full frame spec:
[`../reference/ws-protocol.md`](../reference/ws-protocol.md) (*The `expose`
plugin*).

### A. Inside a VS Code plugin webview → `vscode.postMessage`

A plugin's `ui/index.html` talks to the backend **through the extension bridge**
(`acquireVsCodeApi()` → the extension forwards to `exchange_instance` and posts
`status`/`preview` back). Wire the components' events to that bridge:

```html
<script type="module" src="./xi-components.esm.js"></script>  <!-- vendored, see below -->

<xi-slider id="thr" data-param="threshold" label="Threshold" min="0" max="255"></xi-slider>
<xi-image-viewer id="view" style="width:100%;height:320px"></xi-image-viewer>

<script type="module">
  const vscode = acquireVsCodeApi();
  // control → backend
  document.getElementById("thr").addEventListener("change", (e) =>
    vscode.postMessage({ type: "exchange", cmd: { command: "set_threshold", value: e.detail.value } }));
  // backend → viewer
  window.addEventListener("message", (e) => {
    const m = e.data;
    if (m.type === "preview") document.getElementById("view").setFrame("data:image/jpeg;base64," + m.jpeg);
    if (m.type === "status" && m.threshold != null) document.getElementById("thr").value = m.threshold;
  });
  vscode.postMessage({ type: "exchange", cmd: { command: "get_status" } });
</script>
```

Keep the `data-param`/`data-action` attributes (see
[`write-a-plugin.md`](./write-a-plugin.md#instance-ui-conventions)) so UI tests can
drive your controls by name.

### B. Standalone / HMI / external webapp → `XiClient`

Outside the webview you have a direct WebSocket. Use the shim:

```js
import { XiClient } from "./xi-components.esm.js";
const client = await new XiClient("ws://127.0.0.1:7823/").connect({ checkVersion: /\d+\.\d+\.\d+/ });
slider.addEventListener("change", (e) => client.setInstanceDef("inst0", { threshold: e.detail.value }));
client.onExpose((f) => viewer.setFrame(f.images[0]?.dataUrl));
client.subscribe(["lane"]);   // ← expose channels (see note below)
// ...and when the view closes: client.unsubscribe(["lane"]);
```

> **Legacy (preview removed from core).** The `subscribe`/preview path the
> snippet above replaces no longer has a backend source: the old `subscribeImage` /
> `unsubscribeImage` / `onPreview` helpers mapped onto the removed core `subscribe`
> command and binary preview frame. The control + `status`
> calls still work; live frames now come from the shipped `expose` plugin — its
> `subscribe`/`unsubscribe`/`get` exchange commands and the atomic `XEX1` frame
> (`onExpose` / `decodeExposeFrame`). The decode-once / per-record dedup
> behavior described next was part of that same removed path — retained here as
> background.

### C. Auto-UI from a manifest descriptor → `mountPanel`

Declare your tunables in `plugin.json`'s manifest and get a wired panel for free
(see the schema in [`../reference/c-abi.md`](../reference/c-abi.md) §1):

```js
import { mountPanel } from "./xi-components.esm.js";
await mountPanel(document.getElementById("panel"), {
  client, instance: "inst0",
  descriptor: [{ section: "Tuning", tag: "control", controls: [
    { type: "slider", key: "threshold", label: "Threshold", min: 0, max: 255 },
  ] }],
});
```

`mountPanel` reads the current def, renders the widgets, and writes changes back
via `set_instance_def`. Omit `descriptor` and it's inferred from the def. (In a
webview, pass a small `client` adapter whose `getInstanceDef`/`setInstanceDef` post
through `vscode.postMessage` instead of a WS.)

---

## Teach UIs (draw on the image)

For setup that must be drawn — a template ROI, a measure region, a fiducial — use
`xi-image-editor`; the result comes back via `exchange` (the webui drives, your
plugin stays passive — see [`plugin-caveats.md`](./plugin-caveats.md)):

```js
const ed = document.querySelector("xi-image-editor");
ed.setTool("polygon");
ed.setFrame(frameDataUrl);
ed.addEventListener("commit", (e) =>
  vscode.postMessage({ type: "exchange", cmd: { command: "set_template", ...e.detail.result } }));
```

---

## Vendoring the bundle into your plugin

A plugin's `ui/` is served as static files, so copy the built bundle next to your
HTML and load it relatively:

```bash
cp ui-components/dist/xi-components.esm.js  plugins/<your-plugin>/ui/
```
```html
<script type="module" src="./xi-components.esm.js"></script>
```

(The release bundle ships it too.) Regenerate with `npm run build` in
`ui-components` after pulling library updates.

---

## See also

- [`write-a-plugin.md`](./write-a-plugin.md) — authoring the plugin + its UI conventions.
- [`plugin-caveats.md`](./plugin-caveats.md) — UI patterns (UX in the webui not C++, geometry).
- `ui-components/README.md` — the library reference + every export.
- `ui-components/demo/` — the playground pages above.
