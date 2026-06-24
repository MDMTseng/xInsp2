import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

// Library build: emit an ESM module (for npm / library-import) AND a single
// drop-in UMD bundle (xi-components.js) for offline / vanilla <script> use. The
// Svelte build step is contained HERE; consumers load the built custom elements.
export default defineConfig({
  plugins: [svelte()],
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
