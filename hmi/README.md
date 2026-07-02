# xInsp2 Production HMI (RUN + Compose modes)

A standalone browser SPA operator dashboard. It is the **single WS client** of a
(FE-supervised) backend: it consumes the backend's live **event stream**
(`run_result` / `run_finished` / `status` events + `dispatch_stats`) and renders a
grid of cards described by `dashboard.json`. It carries **no** vars/image-preview
decoding — surfacing per-run values and images is a plugin's own job (the shipped
`expose` plugin's subscribe-gated XEX1 transport, rendered by that plugin's webUI),
not the HMI's. No build step — plain ES modules. Design:
[`../docs/roadmap/production-hmi.md`](../docs/roadmap/production-hmi.md).

> **Version:** there is no version constant in this folder (no `hmi/package.json`).
> Functionally the HMI today ships **both** RUN mode (render a live dashboard) and
> **Compose mode** (edit the layout in-page, export the JSON) — see **Scope** below.

## Try it (live demo)

```sh
python hmi/serve.py
```

This spawns the backend headless on `hmi/demo/` (continuous via `--autostart-fps`),
serves this folder over HTTP, and opens the URL. You should see the demo
`dashboard.json` cards updating live: an OK/NG **verdict**, a **yield** %, a
completed-parts **throughput** rate, and (on the Groups tab) live **dispatch
groups** — all fed from the backend event stream. (Windows; backend must be built.
The HMI is the sole WS client — close any VS Code session on the same backend port
first.)

Point it at any backend instead:

```
index.html?ws=ws://<host>:<port>/&dashboard=./dashboard.json
```

## How it works

| File | Role |
|---|---|
| `index.html` | shell: top bar + connection badge + grid container |
| `app.mjs` | WS host — connect, consume the generic event stream, lay out the grid, `feed()` cards |
| `lib/xi-components.esm.js` | vendored ui-components bundle: the `xi-*` elements, cards, and layout engine |
| `dashboard.json` | the layout: a **split-pane tree** of `{dir:"row"\|"col",weights,children}` nodes with `{card:{type,bind,config}}` leaves |
| `demo/` | a source-less project whose script emits a synthetic live stream |

The cards, layout engine, and `XiClient` now live in the shared
[`ui-components`](../ui-components/) library; the HMI vendors the built bundle.

**Data model:** the HMI is a **generic dashboard host** — it consumes only the
`run_result` / `run_finished` / `status` events and `dispatch_stats`, and feeds
the built-in `verdict` / `yield` / `throughput` / `groups` cards. It carries
**no** preview/vars/gid decoding — that path was **removed from the HMI**. Per-run
values and images are surfaced by the shipped `expose` plugin over its own
subscribe-gated **XEX1** binary transport (a consumer subscribes by *channel*; the
plugin JPEG-encodes + pushes only subscribed channels), decoded and rendered by
**that plugin's own webUI**, not here. See
[`../docs/roadmap/expose-plugin-and-output-transport.md`](../docs/roadmap/expose-plugin-and-output-transport.md).

**Dashboard source:** on connect the HMI asks the backend for the *project's*
dashboard (`cmd:get_dashboard` → `<project>/dashboard.json`, or
`dashboard.<name>.json` via `?board=<name>`). If the project has one it wins; if
not, the HMI falls back to the static `?dashboard=…`/`dashboard.json` it was served.
So in production the HMI needs **only the BE WS URL** — it pulls the dashboard *and*
the data from the backend, no filesystem coupling.

```
test:  cd ui-components && npm test      # cards/layout/client tests live with the library
```

## Scope

- **Shipped — RUN mode:** render a split-pane `dashboard.json` against a live
  backend; the built-in generic cards (`verdict` / `yield` / `throughput` / `groups`).
- **Shipped — Compose mode:** toggle **✎ Compose** in the header, then per
  pane: **+⬌ / +⬍** add a pane (an N-pane row/column, each weight set by dragging
  the blue dividers), **⊞** wrap the pane in **tabs/pages** (add `+` / remove `✕` /
  rename by double-click, switch by clicking), pick card type, set its `var` +
  title, **✕** remove. **Copy JSON** / **Download** exports the edited
  `dashboard.json` (`?mode=compose` starts in edit mode). Cards stay live while
  editing. The layout tree has three node kinds: `{card}`, `{dir,children,weights}`,
  `{tabs:[{name,child}],active}` — all nest freely.
- **Not yet:** persist via a backend `save_dashboard` (today you Copy/Download
  the JSON and drop it in the project), vector overlay layers, plugin-shipped
  cards/overlays, single-file build + the AOT production package. See the design doc.

## Serving over a tunnel

`serve.mjs` is the tunnel-friendly runner: one Node process serves the page **and**
reverse-proxies the WebSocket at `/ws` to the backend, so the page uses a
**same-origin** `wss://…/ws` and a **single HTTP tunnel** exposes everything.

```sh
node hmi/serve.mjs                       # http://127.0.0.1:8770  (page + /ws proxy)
# then, via WebTunnelHub (one port):
cd ../WebTunnelHub
./hub-managed-tunnel.sh --name xinsphmi --note 'xInsp2 HMI demo' --port 8770
#   -> https://xinsphmi.db.xception.tech:1080/   (Caddy passes WS through to /ws)
```

## Caveats & gotchas (learned during bring-up)

- **Single-client backend.** The backend WS accepts **one** client. `serve.mjs`
  opens one backend connection **per browser**, so a second tab / a stale tab
  contends for the slot. When it loses the race the backend replies `503
  single-client-busy`; `serve.mjs` relays that to the page as a distinct WS close
  code and the badge shows **"another client is connected"** (not a generic
  "disconnected") while it keeps retrying — so the operator knows to close the
  other tab, not to go restart a backend that is running fine (review 10 #6). (A
  browser connecting **directly** — `?ws=…`, no proxy — can't read the 503 HTTP
  status, a platform limit, so that path still shows a generic retry.) For
  multi-viewer (operator wall + an office screen), v1.1 should make the proxy hold
  **one** upstream and **fan-out/broadcast** to N browsers.
- **Browser ESM runtime errors are invisible to `node --check`.** A stale
  call-site after a rename (`buildGrid()` → `buildLayout()`) threw a *ReferenceError*
  at module load, so `connect()` never ran and the badge sat on its default
  "connecting…". `node --check` only catches **syntax**, not runtime refs. The
  on-page **diagnostics panel** (bottom of the page — logs WS URL + every state +
  caught `window.error`/`unhandledrejection`) is what made this visible without a
  remote DevTools; keep it.
- **Windows `temp_directory_path()` throws if `TEMP` doesn't exist.** When
  spawning the backend with an isolated `TEMP`, **`mkdir` it first** or the backend
  dies at startup with exit `3765269347` (`0xE06D7363`, an unhandled C++ exception).
  Don't spawn the child with `stdio:"ignore"` while debugging — you lose the reason.
- **One same-origin `/ws` proxy beats two tunnels.** Tunnelling page + WS as two
  apps means cross-origin `ws=` juggling; proxying `/ws` on the page's own origin
  is one tunnel, no `?ws=` needed. `app.mjs` defaults to `wss://<host>/ws` and only
  needs `?ws=` for a direct/local backend (e.g. `serve.py`).
- **`ws` (npm) is borrowed from `vscode-extension/node_modules`** via `createRequire`
  (ESM bare-imports ignore `NODE_PATH`); `hmi/` has no `node_modules`.
- **Caddy on :1080 serves HTTP/1.1**, so browser WS upgrades work like the CLI's.
  If h2 is ever enabled, WS-over-h2 needs RFC 8441 (Extended CONNECT) end-to-end.
- **`perMessageDeflate: false`** on the proxy WS server — avoids compression
  negotiation surprises; the stream is small JSON + JPEG already.
- **Static served `no-store`** so the browser can't run a stale `app.mjs`.
- **Restarts orphan the demo backend.** `Stop-Process -Force` skips `serve.mjs`'s
  SIGTERM handler, so the child backend on **:7872** is left running; kill it
  separately on restart. **Never touch :7823 — that's the dev VS Code backend.**
- **Throughput card reports COMPLETED-PARTS rate over a wall-clock window.** It
  used to derive parts/min from `run_finished.ms` (inspect COMPUTE duration) as
  `60000 / avg_ms`, so fast compute showed an unrealistic rate even when triggers
  arrived slowly (external review 05 #20). It now counts terminal `run_result`
  events (one per completed run, run_id deduped) over a rolling wall-clock window
  (default 60s, `config.windowSec`) and reports the real completed rate; inspect
  compute is shown only as a secondary, honestly-labelled readout. Note
  `run_finished.ms` and the additive `run_finished.inspect_compute_us` field are
  inspect **compute** time only, NOT cycle/decision latency — do not derive
  production rate from them.
- **The diagnostics panel overlaps the bottom cards.** It's a bring-up aid; make it
  collapsible (or gate behind `?debug=1`) before this is a real operator screen.
