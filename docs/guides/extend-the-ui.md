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
                     │     ├─ TreeView provider                  │
                     │     │    instanceTree                      │
                     │     ├─ WebviewViewProvider                │
                     │     │    viewerProvider                   │
                     │     ├─ WebviewPanel(s)                    │
                     │     │    pluginBrowser, pluginUI,          │
                     │     │    imageViewerPanel                  │
                     │     ├─ Data cache                          │
                     │     │    pluginRegistry (no view)          │
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
     Gate buttons that act on the inspection script with the
     `xinsp2.isActiveScript` context key (true when the active editor is the
     open project's script, resolved from `project.json`'s `script` field) —
     **not** a hardcoded `resourceFilename == inspect.cpp`, which breaks
     for projects that name their script anything else.
   - **Status bar**: `vscode.window.createStatusBarItem(...)` from
     `extension.ts`.

Existing examples to mimic:
- Recording status bar in `extension.ts` (`xinsp2.startRecording` /
  `xinsp2.stopRecording` toggle).
- The Instances title overflow groups `1_plugins` / `2_project` —
  non-`navigation` groups collapse into the view's `⋯` menu, keeping only
  the high-frequency icons (add instance, run/stop, pipeline graph) inline.

---

## Adding a tree view item

The only tree view is `xinsp2.instances` (left of the activity bar).
Plugins are **not** a tree — they're managed in the **Plugin Browser**
webview panel (`pluginBrowser.ts`, opened by `xinsp2.pluginBrowser`);
the last-known plugin set is just cached in `pluginRegistry.ts` (a plain
data holder, no view) to feed that panel's live loaded/use status.

To add a new top-level tree:
1. Implement `vscode.TreeDataProvider<T>` in a new
   `src/<thing>Tree.ts`.
2. Register in `extension.ts` with
   `vscode.window.createTreeView(...)`.
3. Declare in `package.json` under `"contributes" → "views"`.
4. Add a Welcome view in `viewsWelcome` for the empty state.

To add an item type to an existing tree: edit `instanceTree.ts`'s `Node`
union, render it in `getTreeItem`. Set `contextValue` so menu predicates
can match it.

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

### Plugin instance-UI message contract (exact)

The webview ↔ extension protocol for `<plugin>/ui/index.html`:

**Webview → extension** (`acquireVsCodeApi().postMessage(...)`):

| Post | Effect |
|---|---|
| `{ type: 'exchange', cmd }` | Calls `exchange_instance(name, cmd)`. **`cmd` may be a string OR an object** — an object is JSON-serialized on its way to the plugin's `exchange()`. The reply comes back on the **status** channel (below). |
| `{ type: 'request_process', cmd? }` | Runs `exchange_instance` (defaults to `{command:'get_status'}`); reply arrives as `{ type: 'process_result', ... }`. |

**Extension → webview** (`window.addEventListener('message', …)`):

> ⚠️ The extension **overwrites `type`** on exchange replies. An `exchange`
> reply is delivered as `{ type: 'status', ...<your reply JSON, spread> }`, and a
> `request_process` reply as `{ type: 'process_result', ...<reply> }`. So your
> plugin's `exchange()` JSON should **not** rely on its own `type` field (it's
> clobbered) — branch on a distinctive field instead (e.g. `msg.jpeg`,
> `msg.results`, `msg.features`). The initial `get_status` is pushed the same way
> (`{ type: 'status', ... }`).

For a worked example of a webview that does a **WS round-trip plus a native
dialog**, see the **C++ Toolchain** section in `xinsp2.openProjectSettings`
(`renderProjectSettingsHtml`): the webview posts `tc_refresh` / `tc_set`, the
extension answers with `toolchain_health` over WS, drives
`vscode.window.showOpenDialog` for path picking, writes the choice via
`set_toolchain_override`, and posts the refreshed `tc_health` back. See
[`build-and-run.md`](./build-and-run.md) §5 + [`../reference/ws-protocol.md`](../reference/ws-protocol.md) →
`toolchain_health`.

---

## Adding a webview "thing the script will use" — the inline image viewer

The `imageViewerPanel.ts` standalone viewer + the inline preview widget
(currently inlined in the project plugin templates) both implement the
same pan + cursor-anchored zoom math. The widget still takes a JPEG/base64
frame and draws it — but note the **core image-preview transport that used to
feed it (`vars`/binary preview frames) has been removed**; the shipped `preview`
plugin is the frame source now (it pushes binary preview frames via the ABI v8
`emit_binary` path). If you need a new image preview location:

1. For a **separate tab**: call `ImageViewerPanel.show(extensionUri,
   { name, width, height, jpegBase64 })`.
2. For an **inline preview inside another webview**: copy the widget
   block (CSS + div + script) from
   `vscode-extension/src/imageViewerPanel.ts`. Element id = `preview`
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

## Decorating + linking script symbols (`xi::use("…")`)

Two related features share one scanner (`scanUses` + the `USE_RE` regex over
`xi::use("name")`) and the `instanceMap` (name→plugin,
refreshed from each `instances` message):

- **Instance highlighting** — two `TextEditorDecorationType`s colour the instance
  name: known instances (in `instanceMap`) one way, unknown/typo'd names in
  `errorForeground` (catches typos). Refreshed on active-editor / text change and
  on the `instances` message. Decorations are the simplest route for colour you
  control; reach for semantic tokens only if you need theme-grammar integration.
- **Ctrl/⌘+click → webui** — a `DocumentLinkProvider` for `cpp` turns each known
  instance name into a link whose target is a `command:` URI
  (`command:xinsp2.openInstanceUI?<json-args>`). Clicking runs the command;
  `openInstanceUI` resolves the plugin from `instanceMap` when only the instance
  name is passed. `command:` URIs in `DocumentLink`s execute directly (no
  `isTrusted` dance needed, unlike markdown links).

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

Existing settings: `backendPort`, `autoStartBackend`, `backendMode`,
`feStatusFile`, `extraPluginDirs`, `remoteUrl`, `authSecret`, `autoRespawn`,
`sdkPath`. Look at how each is read in `extension.ts` for the right
pattern.

---

## Backend ownership: managed vs attach

The extension can either **own** the backend process or **attach** to one
owned by the `xinsp-fe.exe` supervisor (the production frontend — see
[`../internals/fe-be.md`](../internals/fe-be.md)). This is the
`xinsp2.backendMode` setting:

- **`managed`** (default) — the extension spawns the backend and respawns it on
  crash (rate-limited 5/min). The dev inner-loop default.
- **`attach`** — a backend is already running (FE-owned on a line). The
  extension connects read/operator-only: it **never** spawns or respawns, and
  `xinsp2.restartBackend` just reconnects (the FE owns the process). On a
  dropped connection the health status bar shows `$(shield) xInsp2 · down`,
  signalling the FE is recovering (respawning) the backend.
- **`auto`** — `isPortOpen(port)` decides: attach if a backend is
  already listening, else managed.

**FE status channel (`xinsp2.feStatusFile`).** Inferring "backend down" from a
WS disconnect can't tell a transient respawn from a latched
`RespawnLimitExceeded`. Point this setting at the supervisor's `fe-status.json`
(the FE writes it next to its `--be-log`; see
[`../internals/fe-be.md`](../internals/fe-be.md)) and the extension polls
it (1.5s) to drive the health indicator from the FE's **true** state:
- `down (n/max)` — recovering, with the respawn budget (warning background);
- `$(error) xInsp2 · LATCHED` — the FE gave up; the tooltip carries the reason +
  last-crash forensics and the disconnect toast switches from "recovering" to
  "manual restart required" (error background);
- `offline` — cleanly stopped.
The live WS connection stays authoritative while connected; the file only drives
the indicator while disconnected. No file configured → falls back to the
disconnect-inference behavior above.

When adding lifecycle UI (anything that spawns/kills/restarts the backend),
**guard it with `attachMode`** so it can't fight the FE supervisor. The
respawn path (`_spawnAndWatch`) is only wired in managed mode; the status-bar
text and the crash messaging branch on `attachMode` too.

---

## Component status (the status channel)

Each plugin instance and the script can publish a sticky status string
(`xi::status(...)` / `xi::Plugin::status(...)`; see
[`../reference/ws-protocol.md`](../reference/ws-protocol.md) "Status channel"). The extension renders it:
the instance tree shows `<plugin> · <status>` per instance and the live script
status on the script item (`InstanceTreeProvider.setStatuses`; the item's
label follows the project's `script` name).

Wiring (in `extension.ts`): a retained `statusMap` is **re-pulled via
`cmd:status` on every `client.on('open')`** (the delivery guarantee), and the
`status` event updates it live. If you add another status consumer (e.g. a
status-bar element), feed it from the same `statusMap` and `refreshStatuses()`
so it survives reconnects — don't rely on the event alone.

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
| Add a tree provider | `instanceTree.ts` |
| Add a webview view | `viewerProvider.ts` |
| Add a webview panel | `pluginBrowser.ts` / `imageViewerPanel.ts` |
| Plugin UI panel | The medium-template `ui/index.html` + the
  `xinsp2.openInstanceUI` handler in `extension.ts` |
| File watcher → compile | The auto-recompile-on-save block (search
  for `onDidSaveTextDocument`) |
| Status bar item | The recording status bar (search for
  `recordingStatus`) |
| Crash recovery hook | `intendedRunning` / respawn block in
  `extension.ts` |
