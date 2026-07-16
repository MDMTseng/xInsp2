# 31 — UI egress and plugin↔UI conventions (decided with CT, 2026-07-14/15)

The external data-flow architecture after the blob plane (doc 30). Everything
here is convention + lib-plugin composition; the core gains NOTHING.

## The three-way rule

> **Product** — what downstream or the record needs → the pack `process()`
> returns. Flows the graph; recorded; deterministic; replayable.
> **Live UI data** — what only human eyes need → the plugin author PUSHES
> explicitly from inside `process()` (see `xi.ui.egress`). No push → no live
> update. The core never taps, samples, or previews anything on its own.
> **State** — what a panel shows when it opens → pulled via `exchange`.

Anti-patterns both directions: painting overlays into the product image
(pollutes the record, forces UI semantics on consumers), or emitting a
measurement only to the UI channel (the record silently loses it).

## Plugin ↔ its own UI (control plane — expose is NOT involved)

| verb | semantics | status |
|---|---|---|
| `exchange` | synchronous request/response between a plugin and its panel (`{type:'exchange',cmd}` ↔ `{type:'status'}`, `data-param`/`data-action`) | shipped; the panels in xInsp/plugins already use it |
| `set_param` | single-parameter set: raw JSON → plugin validates (0/-1/-2) → param_cache → `save_project` solidifies | shipped |
| `set_instance_def` / `get_instance_def` | whole-config in/out (product/instrument bundles, `image_png_b64` templates) | shipped |

These ride the service cmd plane (WS JSON, id-correlated). A `$req`-style
async-reply convention was considered and DROPPED — `exchange` covers the
sync case; an async action page (progress/cancel) is deferred until a real
consumer needs it.

## Live UI data: `xi.ui.egress` (service-middleware lib plugin)

```
producer process():  ui.push(chan, image/pack)   ← one line, resolved cap
      │  (writes a latest-wins slot and returns; never blocks the lane)
      ▼
xi.ui.egress lib plugin (cap provider; NO graph identity, NO inspect.cpp wiring)
      own timer thread @ UI rate → dedup (handle-keyed LRU) →
      dispatch by descriptor "t" → xi.jpeg.encode (cap) → hand to expose
      ▼
expose = pure transport: channel subscriptions + WS fan-out, drop-not-queue
```

- Cap absent → push is a fail-open no-op (the project's plugin list decides
  whether live UI exists at all).
- Channel naming: `ui/<instance-id>` is the default panel channel.
- Dispatch defaults: `xi/image` u8 → jpeg q80; `xi/jpeg` → pass-through; >2MP →
  box-downscale to ≤1MP first; unknown `"t"` → metadata card. All expose/egress
  CONFIG, never ABI. (E1 ships a SINGLE-GLOBAL config — the per-channel override
  table is DEFERRED until a real consumer needs divergent per-channel policy.)
  **`f32`/`u16` → normalize→u8 (or PNG16) is DEFERRED** — E1 shows those as a
  metadata card (the code comment says "we don't normalize yet"); implement when
  a real non-u8 preview consumer exists.
  **`encode:false` (config) = RAW PASSTHROUGH** — the "this channel walks raw"
  ruling: the flusher ships the pushed blob verbatim (no jpeg/downscale/LRU), so
  push AND pull both see raw at raw's honest cost. Global today; the per-channel
  table inherits it. Per-REQUEST raw is deliberately NOT egress's job — that is
  State (an `exchange` ask to the plugin), see the three-way rule.
- Dedup key = **content hash** (FNV-1a over descriptor + payload) folded with the
  policy fingerprint (quality + downscale target), NOT the pool handle: the @4
  `get_blob` door surfaces descriptor + payload spans but not the handle (the same
  ABI reason xi.imgcodec hashes content), and sealed buffers are immutable so
  content IS identity. This also sidesteps the handle-recycle/generation ABA a
  raw-handle key would have. The LRU caches the ENCODED byte copies (not a
  retained pack); bounded (32 entries default), evicts LRU.
- Codec caps are one-per-encoder (`xi.jpeg.encode`, later `xi.png16.encode`…),
  PURE encode — scaling/normalizing policy stays in egress. A future
  **video arm** (`xi.video.encode`, stateful sessions, WebCodecs client) is
  design-settled in `docs/roadmap/video-egress.md` — need-driven, ladder
  `h264(hw) → jpeg → raw`.
- Egress is the first STATEFUL, own-threaded cap service: it alone carries the
  OwnerGuard + teardown discipline (slot packs released on reload/teardown);
  producer authors never learn it.

## Hosts: everyone is just a WS client

- **VS Code extension** (dev): param panels auto-generated from the plugin
  manifest def; teach via `xi-image-editor` → exchange; live view = subscribe
  `ui/<instance>`; webview bridge is a host detail.
- **App-built webapps** (production; BE runs headless): `XiClient` + the
  `ui-components` library + each plugin's vendored `ui/index.html` running
  unchanged behind the official **`acquireVsCodeApi→XiClient` shim**
  (integration_test FR-3). The app owns composition and interaction logic.
- **Headless harnesses**: same WS, any language.

Component delivery: compiled into each host (repo workspace; no npm publish,
no runtime module loading — a `ui-registry@N` frozen JS surface is deliberately
NOT opened; revisit only when an out-of-repo toolbox ecosystem exists).
The declarative renderer library (image / heatmap / profile / overlay / table /
hex, driven by descriptor keys) covers the no-code path.

## Product data to the outside

Explicit graph wiring of expose as a sink (as today) — record files, result
streams for webapps. Not core magic, not egress.

## Sugar boundaries (standing rulings)

- Per-type SDK sugar stops at `xi/image` (doc 30). Other types: descriptor
  keys + `padded_layout`/`place_padded_head` mechanics, toolbox-owned.
- No `out.ui(...)` producer sugar yet (a one-line cap call doesn't earn it).
