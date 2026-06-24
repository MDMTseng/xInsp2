# WebUI component library + section-based UI export — design sketch

> **Status: design only, not implemented.** Converged in the 2026-06-24 session.
> Captures the decision for (a) how a plugin gets a usable webui with little/no
> work, and (b) how a finished xInsp2 project exports its UI — for two distinct
> delivery shapes. Supersedes / absorbs
> [`interactive-tool-registry.md`](./interactive-tool-registry.md) (the
> draw-on-image editor is one component in this library). Graduates to a guide +
> `reference/` when it ships.

## The need — two app shapes (illustrated by `integration_test/test1`)

`xInsp/integration_test/test1` is a real end-to-end lane (cmd_panel → cal0 lens
calib → sbm0 match → posereg → meas0 measure). It is **not** something we build
or change — it's the worked example that shows the two ways a finished project is
delivered, which is what drives the decisions below:

- Each plugin has its **own teach/observe UI** (`ui/index.html`): sbm0 draws a
  template ROI, meas0 adds features+tolerances, cal0 sets the board, cmd_panel is
  a hand-written "one GUI". Some are interactive setup; some are status readouts.
- A **host-layer app** (`app.py`, a WS client) sits above the lane and switches
  config bundles (`use_instrument` / `load_product` / `inspect` over
  `get_instance_def`/`set_instance_def`). "A real HMI would call the same verbs."

From that, the two delivery shapes:

| | **A — status/observation export** | **B — full-app extract** |
|---|---|---|
| Scenario | The dev environment IS the deliverable; you just want to *watch* it run. | The project is a pipeline + a separate operator webapp for setup & operation. |
| Exports | Selected **sections** (mostly `status`) across plugins → a read-only monitoring page. | The complete settings + flow, curated into an operator app (cmd_panel-style main + bundle switching + teach UIs). |
| Nature | **Generic, composable, partial-select, read-mostly.** | **Bespoke, hand-curated.** Plugins are deeply customized; NOT required to auto-compose into a perfect generic app. |
| Auto-generated? | Yes — from each plugin's declared sections. | No — assembled by the integrator (test1 is this shape). |

The key realisation: **A is generic and auto; B is bespoke and curated.** We build
infrastructure that makes A free and B cheap-to-assemble — we do **not** try to
auto-generate B.

## Decisions (locked this session)

1. **Default auto-webui from a control-descriptor schema.** A plugin declares its
   tunable keys + control types (in its `plugin.json` manifest); the dev
   environment renders a usable simple UI for free — sliders / number inputs /
   dropdowns / toggles / radios bound to those keys. Zero UI code for the common
   case.
2. **Custom escape hatch stays.** A plugin that wants more drops its own
   `ui/index.html` (vanilla JS or anything) — exactly as cmd_panel/sbm0/meas0 do
   today. The auto-webui is the easy path, not the only path.
3. **The `section` is the unit of composition AND export.** Every plugin webui
   (auto or hand-written) is split into named sections, each tagged by purpose:
   `setup` (interactive teach) / `control` (runtime tuning) / `status` (observe).
   Auto-webui derives sections from the descriptor groups; a hand-written UI
   declares its own section boundaries + tags.
4. **Output format = Web Components; Svelte is an internal authoring detail.**
   The `xi-*` widgets compile to standard custom elements. **Svelte is allowed
   inside the component-library repo** (its only build step); **every consumer —
   the HMI's vanilla `.mjs`, plugin `ui/index.html`, VS Code webviews — stays
   framework-free** and just uses `<xi-slider key="…">`. The no-toolchain ethos
   holds for everyone except the library itself.
5. **Two export modes, not one auto-everything.** A = pick `status` sections →
   read-only monitor bundle (generic). B = curate sections + the app shell +
   bundle-switching into a bespoke webapp (integrator's job, test1-style).
6. **Integration boundary = custom elements (+ Shadow DOM), NOT iframes.** A plugin
   UI lives in the host's document, so it can **tap out a rich API** — properties
   (`el.value`), methods (`viewer.fit()`, `tool.startTool('polygon')`), and custom
   events (`change`, `pixelpick`, `commit`) the host calls/listens to directly — vs
   an iframe's postMessage-only channel (serialize everything, hand-roll a protocol
   per widget). The cost is no JS isolation, but the project's trust model already
   assumes **trusted plugins** (trusted DLL load, no cert gate, no hostile-plugin
   defenses), so iframe isolation is low-value here. **Shadow DOM** still gives
   style/DOM encapsulation (no CSS bleed either way) WITHOUT giving up the JS API —
   it decouples "don't pollute each other" from "JS sandbox", and we only want the
   former. Sandboxed `iframe` stays the tool for genuinely **untrusted** third-party
   UIs (a future hybrid: trusted = custom element, untrusted = sandboxed iframe);
   not a current need. This also means library-import (model 2 below) hands an
   external app the full JS API, not a postMessage shim.

## The component library (`xi-*` web components)

One small library, many consumers (dev webui, export A, export B, production HMI):

- **`xi-image-viewer`** — image display with zoom / pan / cursor-anchored zoom /
  pixel probe, fed by the WS binary preview frames. The pan/zoom math already
  exists (`imageViewerPanel.ts` + its selftest) — this generalises it.
- **Bindable controls** — `xi-slider` / `xi-number` / `xi-toggle` / `xi-radio` /
  `xi-dropdown`, each `key`-bound: a change patches that key via `set_instance_def`
  (or fires an `exchange` command). Persist the binding (`key` name, range,
  options) so a panel is configured once, not re-built every session.
- **`xi-trace`** — watch/plot an output key across runs (the "key trace").
- **Interactive teach tools** — the draw-on-image editors (polygon, ROI mask,
  fiducial pick) from [`interactive-tool-registry.md`](./interactive-tool-registry.md):
  a tool registry + a host hook (`xi_host_request_editor`) so a plugin can ASK the
  UI to open the viewer with a tool active and get the result back via `exchange`.
  That sketch is now **one part** of this library (the `setup`-section tooling).

## Control-descriptor schema (starting point)

A flat list in the plugin's `plugin.json` manifest (the existing `manifest_json`
free-form slot). Each entry binds a widget to a key:

```jsonc
{ "section": "Threshold", "tag": "control",
  "controls": [
    { "type": "slider",   "key": "threshold", "label": "Threshold", "min": 0, "max": 255, "step": 1 },
    { "type": "dropdown", "key": "mode",      "label": "Mode", "options": ["light","dark"] }
  ] }
```

- `type` → which `xi-*` widget. `key` → the def key it reads/writes (via
  `get_instance_def`/`set_instance_def`). `section` + `tag` → composition/export
  grouping. The renderer is total: an unknown `type` degrades to a number/text box.
- A hand-written `ui/index.html` opts out of auto-render but still declares
  `section`+`tag` (a small `<meta>`/JS export) so export A/B can find its sections.

## Consumption models — export-bundle vs library-import

The `xi-*` widgets are framework-agnostic Web Components shipped as an ESM
package, so the same library is consumed two ways — **two faces of one library**,
not two builds:

1. **Export-bundle** — xInsp2 pre-wires a self-contained static folder
   {selected sections + WS-client shim + component lib} (reusing
   `tools/export_bundle.py` / the HMI AOT path). "I finished in the dev env, give
   me a deployable page." An export-bundle is essentially a *generated* instance of
   model 2.
2. **Library-import** — an **external webapp project** owns its own repo / build /
   stack (React, Svelte, vanilla — Web Components embed anywhere) and just
   `import`s the components (`@xinsp2/components`, or a drop-in `xi-components.js`),
   placing `<xi-image-viewer>` / `<xi-slider key="…">` and driving the backend over
   WS. This is the more general model; the export-bundle is its pre-built case.

Mapping to the two app shapes:

- **A (status)** → the **export-bundle** convenience covers it: partial-select
  `status` sections → a thin read-only monitor page, generic, auto from declared
  sections.
- **B (full app)** → **library-import is the proper delivery.** B is bespoke, so
  its natural home is an external webapp that imports the lib and composes freely.
  B's "export" is therefore **scaffolding an external project skeleton** (WS
  pre-wired, lib imported, an example composition + the bundle-switch verbs) that
  the integrator then owns and develops. `sections` are optional on this path — a
  fully hand-built app places widgets directly.

### What library-import requires of us

- **Publish the lib as importable ESM** — an npm package AND a single drop-in
  `xi-components.js` (industrial sites may be offline; support vendored, not just
  npm).
- **Ship a WS-client shim** with it — so external apps don't reimplement the
  protocol (connect / `set_instance_def` / `prepare_instance` / `commit_group` /
  `subscribe` vars).
- **Descriptors-as-data** — export the control-descriptors as JSON so an external
  app can auto-render the same widgets for an instance, or ignore them and place
  widgets by hand.
- **A scaffold** (`xinsp2 create-webapp` or similar) that seeds the B external
  project.
- **Version binding** — the WS-client shim + protocol are versioned together; an
  external app pins a lib version and fail-fast checks the backend's WS `version`
  on connect (reuse the existing version-check, don't invent a new one).

## Relationship to orchestration (separate primitive)

B's "switch product / station" verbs (the `app.py` layer) are orchestration. They
work **today as a WS client** (app.py proves it: `get_instance_def` save +
`set_instance_def`/`prepare_instance`/`commit_group` load). Letting that logic live
**inside the backend as an orchestrator plugin** needs a small plugin-facing
`host_api` (the open half of RFC #65 FR-4) — **not built, not in this doc's scope**.
The webui/export work here consumes the WS verbs either way.

## Open questions

1. **Descriptor source of truth** — manifest-declared only, or also
   runtime-discoverable (a plugin emits its descriptor via `exchange`)? Manifest is
   the start; runtime lets a plugin vary controls by state.
2. **Binding persistence location** — alongside `instance.json`, or in the export
   bundle? (Ties to the config-set store in
   [`config-bundles-and-orchestration.md`](./config-bundles-and-orchestration.md).)
3. **Section tags** — is `setup`/`control`/`status` enough, or do we need
   per-section roles/permissions (e.g. operator vs engineer visibility)?
4. **Library packaging** — one `xi-components.js` bundle, or per-component ESM so a
   plugin pulls only what it uses? Affects plugin `ui/` weight.
5. **HMI delivery** — does production HMI become "a page composed of these web
   components" (replacing the bespoke `hmi/*.mjs` cards), or keep both?

## Build order (tasks #71–#80)

```
#71 scaffold xi-components (Svelte→web-component build) ┐
#72 WS-client shim ────────────────────────────────────┤
                                                         ▼
#73 PoC: xi-image-viewer + xi-slider over WS  ──┬──────────────┐
                                                ▼              ▼
#74 bindable widgets (number/toggle/radio/dropdown)   #77 teach tools (registry + request-editor)
        ▼                                              #80 adopt in existing consumers (webview + HMI)
#76 control-descriptor schema + section model + auto-webui ──┬──► #78 export A (status monitor bundle)
                                                             └──► #79 export B (library-import + scaffold)
#75 xi-trace  ◄── needs only #72
```

Start = #71 + #72 (parallel) → #73 PoC de-risks the whole stack
(Svelte→custom-element→vanilla-consumer + tap-out JS API + WS round-trip) before
the rest fan out. Library lives at `ui-components/` (parallel to `hmi/`); the build
step is contained there, consumers stay vanilla.

## See also

- `xInsp/integration_test/test1` — the worked two-shape example (illustration, not
  built here).
- [`interactive-tool-registry.md`](./interactive-tool-registry.md) — the
  draw-on-image teach tools, now a sub-part of this library.
- [`production-hmi.md`](./production-hmi.md) — the operator dashboard; a candidate
  consumer of this library.
- [`config-bundles-and-orchestration.md`](./config-bundles-and-orchestration.md) —
  the config-set switching the B app drives.
- `vscode-extension/src/imageViewerPanel.ts` — the pan/zoom math `xi-image-viewer`
  generalises.
