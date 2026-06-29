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
> and `status` are unaffected and remain the live path. **Image surfacing for a
> plugin UI now goes through the shipped `preview` plugin** (`plugins/preview`,
> `sink:true`): its live contract is the `list_groups` / `get` / `get_image`
> exchange commands (pull views) plus `XPV1` binary frames pushed via the ABI v8
> `emit_binary` host call. See
> [`write-a-script.md`](write-a-script.md) and
> [`../reference/c-abi.md`](../reference/c-abi.md) (`emit_binary` v8 /
> `compress_image` v9). The `preview` message + `subscribeImage` examples below are
> kept as the prior pattern for reference.

#### The `preview` plugin's consumer contract

A viewer drives the `preview` instance with three pull `exchange_instance`
commands (latest-record-per-group state) plus a live binary push. From
`plugins/preview/plugin.json` + `src/preview.cpp`:

| Command | Args | Returns |
|---|---|---|
| `list_groups` | — | `{ count, groups: { <pg>: { seen, image_count } } }` — drives the UI's group tabs |
| `get` | `{ pg }` | `{ found, data ($layout + values), image_count }` — the group's latest record |
| `get_image` | `{ pg, key }` | `{ found, w, h, channels, jpeg_b64 }` — a pull still for one image key |

Live images are also **pushed** as self-describing `XPV1` binary frames via the
host `emit_binary` call (ABI v8), one per image as a group's record updates. The
byte layout (little-endian `u16`):

```
[0..3]  'XPV1' (magic)
[4..5]  u16 width      [6..7] u16 height
[8]     u8  channels   [9]    u8  codec (1 = jpeg)
[10]    u8  pg_len     [11]   u8  key_len
[12..]  pg bytes | key bytes | jpeg payload
```

So a frame self-identifies its preview-group (`pg`) and field (`key`) — no
side-channel needed to route it to the right tab/viewer.

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
client.onPreview((f) => viewer.setFrame(f.image || f.dataUrl));
client.subscribeImage("gray");   // ← legacy: core no longer streams previews (see note below)
// ...and when the view closes: client.unsubscribeImage("gray");
```

> **Legacy (preview removed from core).** The `subscribe`/preview path the
> snippet above uses no longer has a backend source: `subscribeImage` /
> `unsubscribeImage` / `onPreview` mapped onto the removed `subscribe` command and
> binary preview frame. The control + `status`
> calls in the same snippet still work; only these image-streaming lines are inert;
> live frames now come from the shipped `preview` plugin (`XPV1` via `emit_binary`).
> The decode-once / `vars.src` dedup
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
