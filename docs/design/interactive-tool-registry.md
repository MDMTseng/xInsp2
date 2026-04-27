# Interactive tool registry — design sketch

> **Status: design only, not implemented.** This is the response to
> "how should plugins that *need* GUI setup (shape-model template
> editing, ROI mask drawing, calibration target capture) wire that
> setup into xInsp2 without each plugin reimplementing pan/zoom/draw
> plumbing?".

## The problem

Some plugin operations are inherently human-interactive. A
shape-based matcher needs the user to outline the target; a ROI mask
plugin needs the user to draw the region; a calibration plugin needs
the user to mark fiducials. The framework already has good primitives
for "user tweaks sliders" (`xi::Param` + plugin webview), but nothing
for "plugin asks the user to draw on an image".

Today's options each have problems:

- **Plugin's own sidebar webview**: cramped, no shared pan/zoom math,
  every plugin reinvents drawing.
- **Plugin opens its own `WebviewPanel`**: better real estate, but
  duplicates the `imageViewerPanel.ts` pan/zoom/screen-to-image
  transform code.
- **`imageViewerPanel.ts` with hardcoded `Point|Area` tools**: shared,
  but plugin authors can't add a `polygon` or `freeform` tool without
  editing the extension.
- **Hand-edit a config file**: terrible UX.
- **External tool**: works but breaks the in-IDE flow.

## Proposed shape

Three pieces. None implemented yet.

### 1. Tool registration in `imageViewerPanel.ts`

Today the panel has a fixed `tool: 'point' | 'area'` set. Make it a
registry the extension and plugins can add to:

```ts
ImageViewerPanel.registerTool({
    id: 'shape_polygon',
    label: 'Polygon',
    icon: '$(symbol-array)',
    overlay: (ctx, state) => { /* draw current polygon */ },
    onPointerDown: (e, state) => { state.points.push(e.imgPoint); },
    onPointerMove: (e, state) => { ... },
    onPointerUp:   (e, state) => { ... },
    onCommit:      (state) => { return { points: state.points }; },
});
```

Pan / zoom / cursor-anchored zoom math stays where it is (already has
a self-test). Each tool is a stateful overlay + event-handler bundle.

### 2. Host-API hook: `xi_request_template_editor`

Plugins are passive today (they wait for `exchange_instance`); they
can't ASK the UI to open a panel. New host call:

```c
int xi_host_request_editor(
    const char* instance_name,
    xi_image_handle frame,    // image to edit on
    const char* tool_id,      // "shape_polygon", "roi_mask", etc.
    const char* opts_json,    // tool-specific (color, vertex limit, ...)
    char* result_json,
    int result_buflen);
```

Backend translates this into a `cmd:open_template_editor` event to
the connected client. The client (extension) opens an
`imageViewerPanel` with the requested tool active. When the user
hits "Commit", the result rides back through `exchange_instance` →
plugin sees the JSON.

Synchronous-looking from the plugin side. Async under the hood;
plugin's calling thread blocks on the reply.

### 3. Standardized commit / cancel

The webview side gets a fixed contract:

- Top-bar "Commit" / "Cancel" buttons (host-rendered, not per-tool).
- Tool-specific overlay + sidebar params live below.
- Commit posts `{ committed: true, tool: '...', result: <tool's onCommit output> }`.
- Cancel posts `{ committed: false }`.

Plugin authors only own the result schema for their tool.

## What this buys

- Pan / zoom / coord transforms written **once** (already done — the
  selftest invariants in `xinsp2.imageViewer.runSelftest` give it
  teeth).
- A shape-model plugin can ship a `polygon` tool **inside its own
  package**, register it on first activation, and reach the editor
  via one host call. No extension changes per plugin.
- AI agents can drive the same path via the existing `applyOp` test
  hook (extend it with `tool` ops).

## What this leaves open

- Drag-and-drop ordering of tool buttons in the toolbar (cosmetic).
- Multi-instance template state — currently `imageViewerPanel` is a
  singleton; opening a second editor would queue or replace.
- Persistence of in-progress edits across panel close — probably
  out of scope; commit-or-discard is simpler and safer.
- Permissions: a malicious plugin could spam editor requests. Cap
  per-second + require user gesture for the first one.

## When to actually build this

When **two** real plugins need it. With one plugin (the first
shape-matcher), it's cheaper to inline the editor. With two, the
duplicated pan/zoom/draw plumbing makes the extraction pay off.

## See also

- `docs/guides/extending-the-ui.md` — current image viewer test hooks
  (`xinsp2.imageViewer.runSelftest` / `applyOp`) that this design
  generalises.
- `vscode-extension/src/imageViewerPanel.ts` — current implementation.
- `vscode-extension/src/imageViewerWidget.ts` — shared widget already
  used in plugin webviews; same pan/zoom math.
