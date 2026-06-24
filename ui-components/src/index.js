// xi-components — library entry. Importing this registers every xi-* custom
// element and re-exports the shared WS client + protocol for library-import
// consumers (external webapps). Build (vite) emits dist/xi-components.esm.js
// (ESM) + dist/xi-components.js (drop-in UMD). See docs/roadmap/webui-and-ui-export.md.

// Side-effect imports: a CE-compiled .svelte module defines its element on import.
import "./components/xi-slider.svelte";
import "./components/xi-image-viewer.svelte";

// Shared client/protocol (no build needed; handy for `@xinsp2/components` users).
export { XiClient } from "./ws-client.mjs";
export * as protocol from "./protocol.mjs";

export const XI_COMPONENTS = ["xi-slider", "xi-image-viewer"];
