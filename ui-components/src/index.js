// xi-components — library entry. Importing this registers every xi-* custom
// element and re-exports the shared WS client + protocol for library-import
// consumers (external webapps). Build (vite) emits dist/xi-components.esm.js
// (ESM) + dist/xi-components.js (drop-in UMD). See docs/roadmap/webui-and-ui-export.md.

// Side-effect imports: a CE-compiled .svelte module defines its element on import.
//
// PLUGINLET-OWNED widgets come from the plets themselves (doc 37: a plet owns its
// UI half as SOURCE; the app layer DEPENDS ON the plet, never the reverse). They
// are discovered from each pluginlet.json's build.ui.widgets by the
// xi-pluginlet-ui vite plugin — adding one needs no edit here:
//   controls  → xi-slider / xi-number / xi-toggle / xi-radio / xi-dropdown / xi-text
//   live-view → xi-image-viewer / xi-image-editor
import "virtual:xi-pluginlet-ui";

// APP-OWNED widgets (this library's own, not tied to a pluginlet).
import "./components/xi-button.svelte";
import "./components/xi-badge.svelte";
import "./components/xi-trace.svelte";

// Shared generic WS client (no build needed; handy for `@xinsp2/components` users).
export { XiClient, BUSY_CLOSE_CODE } from "./ws-client.mjs";
// The official acquireVsCodeApi→XiClient shim (FR-3): run a plugin's VS Code-
// webview ui/index.html unchanged in a plain browser over WS.
export { createVsCodeApi, installVsCodeShim } from "./vscode-shim.mjs";
// Auto-webui renderer: descriptor → sections of wired widgets (task #76), plus
// the manifest param-panel (set_param single-parameter set, doc 31).
export { mountPanel, inferDescriptor, CONTROL_TAGS,
         mountParamPanel, paramsToControls } from "./auto-panel.mjs";
// Declarative renderer library v1 (doc 31 no-code path): descriptor-driven
// image / heatmap / profile / overlay / table / hex renderers.
export { renderDescriptor, pickRenderer, RENDERERS, COLORMAPS,
         imageRGBA, heatmapRGBA, profilePoints, overlayOps, tableRows,
         readScalars, normalize } from "./renderers.mjs";

// Pluginlet runtime mount (doc 37, the third consumer of plugin.json "pluginlets"):
// given a plugin's declared plet list (list_plugins surfaces it), mount each plet's
// UI. The registry is generated from the plet manifests by the build.
export { mountPluginlets } from "./pluginlet-mount.mjs";

// Teach tools (task #77): pluggable draw tools for xi-image-editor.
export { TOOLS, registerTool, makeTool } from "../../toolbox/pluginlets/live-view/ui/lib/tools.mjs";

// Controls plet widget registry (doc 37): the webui's EXPLICIT extension point.
// One line of app wiring — registerWidget("view", factory) — plugs another
// plet's widget (live-view today, a chart plet tomorrow) into the controls
// layout. Owned by the controls plet; re-exported for one-import convenience.
// IDENTITY WARNING: the registry is MODULE-SCOPED. A bundle-consuming app must
// take mountSchema AND registerWidget from THIS bundle — mixing the bundled copy
// with a direct source import gives two registries, and a registration lands in
// the one mountSchema doesn't read (the ⚠ placeholder will say so).
export { mountSchema, registerWidget, unregisterWidget }
  from "../../toolbox/pluginlets/controls/ui/mount-schema.mjs";
// The live-view mount, the canonical thing to register under "view".
export { mountLiveView } from "../../toolbox/pluginlets/live-view/ui/live-view.ui";
// REFERENCE app-custom widget: a plain-Svelte composed panel (TeachPanel) whose
// registration shows the mount()/unmount() + $state-bridge pattern.
export { registerTeachPanelDemo } from "./demo/register-teach-panel.svelte.js";

// HMI dashboard (task #81): importable cards + layout engine + mountDashboard, so
// an external webapp can drop in the whole composable dashboard. Importing cards
// registers the <xi-card-*> elements.
import "./dashboard/cards.mjs";
export { CARDS } from "./dashboard/cards.mjs";
export * from "./dashboard/layout.mjs";
export { mountDashboard } from "./dashboard/dashboard.mjs";

export const XI_COMPONENTS = [
  "xi-slider", "xi-number", "xi-toggle", "xi-radio", "xi-dropdown",
  "xi-text", "xi-button", "xi-badge",
  "xi-trace", "xi-image-viewer", "xi-image-editor",
];
