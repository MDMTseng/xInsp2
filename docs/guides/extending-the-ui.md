# Extending the VS Code UI

Where to add commands, panels, tree items, status bar pieces, and
webview integrations to xInsp2's VS Code extension.

The extension lives at `vscode-extension/`. Bundled by `esbuild.mjs`;
the entry point is `src/extension.ts`. All code is TypeScript.

---

## The big picture

```
                     vscode-extension/
                     ┌──────────────────────────────────────────┐
                     │ src/extension.ts (~2 kLOC)               │
                     │   activate() — wires everything          │
                     │     ├─ WsClient ────────► backend (WS)   │
                     │     ├─ TreeView providers                 │
                     │     │    instanceTree, pluginTree         │
                     │     ├─ WebviewViewProvider                │
                     │     │    viewerProvider                   │
                     │     ├─ WebviewPanel(s)                    │
                     │     │    pluginUI panels, imageViewerPanel│
                     │     ├─ Commands (xinsp2.*)                │
                     │     ├─ DiagnosticCollection (squiggles)   │
                     │     └─ Project state + auto-respawn       │
                     └──────────────────────────────────────────┘
```

Anything that mutates the project goes through a `cmd:*` to the
backend; the UI is downstream of WS state.

---

## Adding a command

1. **Register**:
   ```ts
   context.subscriptions.push(
       vscode.commands.registerCommand('xinsp2.myAction', async () => {
           // your code
       })
   );
   ```
2. **Declare in `package.json`** under `"contributes" → "commands"`:
   ```json
   { "command": "xinsp2.myAction", "title": "xInsp2: My Action",
     "icon": "$(zap)" }
   ```
3. **Surface it** in the right place:
   - **Command palette**: declaration alone is enough.
   - **View title**: add to `package.json` →
     `"contributes" → "menus" → "view/title"`.
   - **Tree item context**: add to
     `"contributes" → "menus" → "view/item/context"` keyed on
     `viewItem == <yourContextValue>`.
   - **Editor title**: `"contributes" → "menus" → "editor/title"`.
   - **Status bar**: `vscode.window.createStatusBarItem(...)` from
     `extension.ts`.

Existing examples to mimic:
- Recording status bar in `extension.ts` (`xinsp2.startRecording` /
  `xinsp2.stopRecording` toggle).
- Project plugin export inline icon (`xinsp2.exportProjectPlugin` on
  `viewItem == projectPlugin`).

---

## Adding a tree view item

Tree views: `xinsp2.instances` (left of activity bar) and
`xinsp2.plugins`.

To add a new top-level tree:
1. Implement `vscode.TreeDataProvider<T>` in a new
   `src/<thing>Tree.ts`.
2. Register in `extension.ts` with
   `vscode.window.createTreeView(...)`.
3. Declare in `package.json` under `"contributes" → "views"`.
4. Add a Welcome view in `viewsWelcome` for the empty state.

To add an item type to an existing tree (e.g. annotate plugins
differently): edit `pluginTree.ts`'s `Node` union, render it in
`getTreeItem`. Set `contextValue` so menu predicates can match it.

---

## Adding a webview panel

Two flavours:
- **`WebviewView`** — embedded in the activity bar's container (e.g.
  the Viewer panel). Implement `WebviewViewProvider`. See
  `viewerProvider.ts`.
- **`WebviewPanel`** — opens as a tab. See `imageViewerPanel.ts`.

Inside the webview HTML, talk back to the extension with
`acquireVsCodeApi().postMessage({ type, ... })`. The extension hooks
`onDidReceiveMessage` to handle it. Other direction:
`panel.webview.postMessage(...)` from the extension lands in
`window.addEventListener('message', e => …)` in the webview.

For plugin webviews specifically: the existing flow at
`xinsp2.openInstanceUI` reads `<plugin>/ui/index.html` and pipes
`{ type: 'exchange', cmd }` posts to the backend's
`exchange_instance` command. Plugin authors don't need to know about
WS; they just write their HTML to talk via that channel.

---

## Adding a webview "thing the script will use" — the inline image viewer

The `imageViewerPanel.ts` standalone viewer + the inline preview widget
(currently inlined in the project plugin templates) both implement the
same pan + cursor-anchored zoom math. If you need a new image preview
location:

1. For a **separate tab**: call `ImageViewerPanel.show(extensionUri,
   { name, width, height, jpegBase64 })`.
2. For an **inline preview inside another webview**: copy the widget
   block (CSS + div + script) from
   `vscode-extension/src/imageViewerWidget.ts`. Element id = `preview`
   by convention; rename if you need >1 in the same webview.

Pan / zoom / coordinate math is identical. Pick tools (Point / Area)
are available only on the standalone panel.

**Test hooks.** Two commands let an e2e drive the standalone viewer
without real mouse events:
- `xinsp2.imageViewer.runSelftest` — runs the cursor-anchored zoom +
  pan + clamp invariants in one shot, returns `{ ok, steps }`.
- `xinsp2.imageViewer.applyOp` — performs a single op
  (`{ kind: 'fit'|'oneToOne'|'zoom'|'pan'|'tool', sx?, sy?, factor?, dx?, dy?, tool? }`)
  and resolves with the post-op transform `{ scale, panX, panY, tool }`.
  Used by `image_viewer_journey.cjs` to screenshot each step.

---

## Sending diagnostics (squiggles)

The extension owns one `DiagnosticCollection` named `'xinsp2'`. To add
a new source of diagnostics:

```ts
const diags = vscode.languages.createDiagnosticCollection('xinsp2-foo');
context.subscriptions.push(diags);
diags.set(uri, [new vscode.Diagnostic(range, message, severity)]);
```

The compiler diagnostics path is in `extension.ts`'s
`applyDiagnostics(diags, sourceCpp)` — copy that pattern (parses
`{file, line, col, severity, code, message}` from the backend's
`compile_and_load` reply, groups by file, dispatches to the
collection).

---

## Hooking into project lifecycle events

`extension.ts` keeps `lastProjectFolder` updated on `cmd:open_project` /
`cmd:create_project`. To run code on project change, find that
assignment site and add your hook there (don't poll).

For "run on every save", the existing watcher pattern at
`onDidSaveTextDocument` is what to copy — debounce 250ms is the right
default.

---

## Settings (configurable)

Declare under `"contributes" → "configuration" → "properties"` in
`package.json`. Read at runtime:

```ts
const cfg = vscode.workspace.getConfiguration('xinsp2');
const port = cfg.get<number>('backendPort', 7823);
```

Existing settings: `backendPort`, `autoStartBackend`,
`extraPluginDirs`, `remoteUrl`, `authSecret`, `autoRespawn`,
`sdkPath`. Look at how each is read in `extension.ts` for the right
pattern.

---

## Status bar items

Two patterns:
- **Persistent** (always shown) — create at `activate()`, set text /
  tooltip / command, call `.show()`. Hide via `.hide()` when
  irrelevant. See the project status item.
- **Transient toast** — `vscode.window.setStatusBarMessage(text, ms)`.
  Auto-dismisses.

---

## Building + reloading

Bundle: `node esbuild.mjs` from `vscode-extension/`.
Watch mode: `node esbuild.mjs --watch`.

Reload extension under a dev host: in VS Code with the extension
project open, press F5 (Run Extension), edit, save, then **Developer:
Reload Window** in the dev host.

---

## Where to look first

For each task type, the closest existing example to mimic:

| Task | Existing example |
|---|---|
| Add a command | `xinsp2.compile` in `extension.ts` |
| Add a setting | `xinsp2.backendPort` declaration + read |
| Add a tree provider | `pluginTree.ts` |
| Add a webview view | `viewerProvider.ts` |
| Add a webview panel | `imageViewerPanel.ts` |
| Plugin UI panel | The medium-template `ui/index.html` + the
  `xinsp2.openInstanceUI` handler in `extension.ts` |
| File watcher → compile | The auto-recompile-on-save block (search
  for `onDidSaveTextDocument`) |
| Status bar item | The recording status bar (search for
  `recordingStatus`) |
| Crash recovery hook | `intendedRunning` / respawn block in
  `extension.ts` |
