# Plugin instance-UI conventions

A plugin's instance UI is plain HTML the plugin ships in `ui/index.html`, loaded
into a VS Code webview (see [`extending-the-ui.md`](./extending-the-ui.md) for the
host wiring and [`adding-a-plugin.md`](./adding-a-plugin.md) for the
`exchange()` contract). These conventions keep that UI **automatable** — by the
plugin UI test harness today, and by any generic param-tuning tooling later.

## `data-param` / `data-action` — stable, name-keyed selectors

Element `id`s are author-chosen and arbitrary (`#thr`, `#close`), so a test or a
generic tool can't find "the control for param `threshold`" without reading the
HTML. Tag each control with the **canonical name** instead:

- **`data-param="<param_name>"`** on each input that edits a parameter. The value
  must match the param's name in the plugin's `exchange()` status JSON (e.g.
  `data-param="close_radius"`, not the `#close` id).
- **`data-action="<action>"`** on each button (`apply`, `reset`, `start`, `stop`).

```html
<input type="range" id="close" data-param="close_radius" min="0" max="12">
<button id="apply" data-action="apply">Apply</button>
<button id="reset" data-action="reset">Reset</button>
```

Keep your `id`s — `data-param`/`data-action` sit alongside them and don't change
your own JS. The new-plugin scaffolds (`sdk/templates/{medium,expert}`) already
emit these; `examples/circle_counting/plugins/region_counter` is a worked example.

### Why

It decouples *selectors* from *ids*, makes the manifest param list the single
source of truth a test can enumerate, and lets a generic harness drive **any**
plugin's UI without per-plugin HTML spelunking:

```js
// helpers.cjs — resolve by param name, not id:
h.setParam('inst0', 'close_radius', 8);   // → [data-param="close_radius"]
h.action('inst0', 'apply');               // → [data-action="apply"]
```

(`h.setInput('#close', 8)` / `h.click('#apply')` still work for id-targeting when
you need it.)

## Driving the UI in a test

Plugin UI tests live in `<plugin>/tests/test_ui.cjs` and run via
`node sdk/testing/run_ui_test.mjs <plugin-folder>` (cold-starts VS Code) or the
**xInsp2: Run Plugin UI Tests** command. The `h` helper drives the real webview
DOM and can screenshot:

```js
module.exports = { async run(h) {
    const proj = h.tmp();
    await h.createProject(proj, 'demo');
    await h.useProjectPlugin(proj);          // compile this plugin from source (see below)
    await h.addInstance('inst0', 'my_plugin');
    await h.openUI('inst0', 'my_plugin');
    h.shot('opened');                        // → tests/screenshots/NN_opened.png
    h.setParam('inst0', 'threshold', 200);
    h.action('inst0', 'apply');
    await h.sleep(150);
    await h.getStatus('inst0');
    h.expectEq(h.lastStatus.threshold, 200, 'threshold round-trips through the UI');
}};
```

## Instantiating an example/source-only plugin: `useProjectPlugin`

Plugins that ship **source but no built+certified DLL** (all the `examples/`
plugins) can't be instantiated via the scan path — the backend's cert gate
rejects them and `create_instance` fails (the error now says so explicitly).
Call **`h.useProjectPlugin(projectFolder)`** right after `createProject`: it
copies the plugin's source into `<project>/plugins/<name>/` and reopens the
project, so the backend compiles it as a **trusted project plugin** (project
plugins are compiled from source and skip the cert gate). This is the supported
way to UI-test an uncertified example plugin.
