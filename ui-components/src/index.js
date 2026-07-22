// xi-components — library entry. Importing this registers every xi-* custom
// element and re-exports the shared WS client + protocol for library-import
// consumers (external webapps). Build (vite) emits dist/xi-components.esm.js
// (ESM) + dist/xi-components.js (drop-in UMD). See docs/roadmap/webui-and-ui-export.md.

// Side-effect imports: a CE-compiled .svelte module defines its element on import.
import "./components/xi-slider.svelte";
import "./components/xi-number.svelte";
import "./components/xi-toggle.svelte";
import "./components/xi-radio.svelte";
import "./components/xi-dropdown.svelte";
import "./components/xi-text.svelte";
import "./components/xi-button.svelte";
import "./components/xi-badge.svelte";
import "./components/xi-trace.svelte";
import "./components/xi-image-viewer.svelte";
import "./components/xi-image-editor.svelte";

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

// Teach tools (task #77): pluggable draw tools for xi-image-editor.
export { TOOLS, registerTool, makeTool } from "./lib/tools.mjs";

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
