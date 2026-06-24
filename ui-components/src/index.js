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
import "./components/xi-image-viewer.svelte";

// Shared client/protocol (no build needed; handy for `@xinsp2/components` users).
export { XiClient } from "./ws-client.mjs";
export * as protocol from "./protocol.mjs";
// Auto-webui renderer: descriptor → sections of wired widgets (task #76).
export { mountPanel, inferDescriptor, CONTROL_TAGS } from "./auto-panel.mjs";

export const XI_COMPONENTS = [
  "xi-slider", "xi-number", "xi-toggle", "xi-radio", "xi-dropdown", "xi-image-viewer",
];
