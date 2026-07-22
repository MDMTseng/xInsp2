import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const PLETS = path.resolve(HERE, "../toolbox/pluginlets");

// ---- pluginlet UI discovery (doc 37) ---------------------------------------
// The webui-build consumer of the plet manifests — the mirror of CMake's
// xi_use_pluginlet on the native side. Each pluginlet OWNS its UI half as SOURCE
// and declares it in toolbox/pluginlets/<name>/pluginlet.json:
//
//   "build": { "ui": { "entry": "ui/…", "widgets": ["ui/widgets/xi-*.svelte", …] } }
//
// This plugin scans those manifests and exposes every declared widget as ONE
// virtual module, so adding/removing a plet widget needs no edit here or in
// src/index.js — the manifest stays the single source of truth.
const VIRTUAL = "virtual:xi-pluginlet-ui";

function pluginletWidgetSources() {
  const out = [];
  if (!fs.existsSync(PLETS)) return out;
  for (const name of fs.readdirSync(PLETS).sort()) {
    const manifest = path.join(PLETS, name, "pluginlet.json");
    if (!fs.existsSync(manifest)) continue;
    let m;
    try { m = JSON.parse(fs.readFileSync(manifest, "utf8")); } catch { continue; }
    for (const rel of m?.build?.ui?.widgets ?? []) {
      const abs = path.join(PLETS, name, rel);
      if (fs.existsSync(abs)) out.push(abs);
      else console.warn(`[xi-pluginlet-ui] ${name}: declared widget missing: ${rel}`);
    }
  }
  return out;
}

function pluginletUi() {
  return {
    name: "xi-pluginlet-ui",
    resolveId: (id) => (id === VIRTUAL ? `\0${VIRTUAL}` : null),
    load(id) {
      if (id !== `\0${VIRTUAL}`) return null;
      // side-effect imports: a CE-compiled .svelte module defines its element
      return pluginletWidgetSources()
        .map((p) => `import ${JSON.stringify(p.replace(/\\/g, "/"))};`).join("\n") + "\n";
    },
  };
}

// Library build: emit an ESM module (for npm / library-import) AND a single
// drop-in UMD bundle (xi-components.js) for offline / vanilla <script> use. The
// Svelte build step is contained HERE; consumers load the built custom elements.
export default defineConfig({
  plugins: [pluginletUi(), svelte()],
  build: {
    lib: {
      entry: "src/index.js",
      name: "XiComponents",
      formats: ["es", "umd"],
      fileName: (fmt) => (fmt === "es" ? "xi-components.esm.js" : "xi-components.js"),
    },
    outDir: "dist",
    emptyOutDir: true,
  },
});
