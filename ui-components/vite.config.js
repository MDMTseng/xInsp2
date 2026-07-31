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
const VIRTUAL_REG = "virtual:xi-pluginlet-registry";

// Read every plet manifest once: { name, dir, manifest }.
function readPluginlets() {
  const out = [];
  if (!fs.existsSync(PLETS)) return out;
  for (const name of fs.readdirSync(PLETS).sort()) {
    const dir = path.join(PLETS, name);
    const mf = path.join(dir, "pluginlet.json");
    if (!fs.existsSync(mf)) continue;
    try { out.push({ name, dir, manifest: JSON.parse(fs.readFileSync(mf, "utf8")) }); }
    catch { /* a malformed manifest is skipped, not fatal */ }
  }
  return out;
}

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

// The RUNTIME half of the same declaration (doc 37): map plet NAME → its mount
// function, built from each manifest's build.ui.entry + halves.ui.symbol. A host
// that knows a plugin's "pluginlets" list (the backend now surfaces it in
// list_plugins) can then mount each plet's UI without hardcoding any plet name.
function pluginletRegistrySource() {
  const imports = [];
  const entries = [];
  for (const { name, dir, manifest } of readPluginlets()) {
    const entry = manifest?.build?.ui?.entry;
    const symbol = manifest?.halves?.ui?.symbol;
    if (!entry || !symbol) continue;
    const abs = path.join(dir, entry);
    if (!fs.existsSync(abs)) {
      console.warn(`[xi-pluginlet-ui] ${name}: ui entry missing: ${entry}`);
      continue;
    }
    const alias = `__plet_${name.replace(/[^A-Za-z0-9_]/g, "_")}`;
    imports.push(`import { ${symbol} as ${alias} } from ${JSON.stringify(abs.replace(/\\/g, "/"))};`);
    entries.push(`  ${JSON.stringify(name)}: ${alias},`);
  }
  return `${imports.join("\n")}\nexport const PLUGINLET_UI = {\n${entries.join("\n")}\n};\n`;
}

function pluginletUi() {
  return {
    name: "xi-pluginlet-ui",
    resolveId: (id) =>
      id === VIRTUAL ? `\0${VIRTUAL}` : id === VIRTUAL_REG ? `\0${VIRTUAL_REG}` : null,
    load(id) {
      // side-effect imports: a CE-compiled .svelte module defines its element
      if (id === `\0${VIRTUAL}`)
        return pluginletWidgetSources()
          .map((p) => `import ${JSON.stringify(p.replace(/\\/g, "/"))};`).join("\n") + "\n";
      if (id === `\0${VIRTUAL_REG}`) return pluginletRegistrySource();
      return null;
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
