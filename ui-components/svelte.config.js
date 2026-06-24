import { vitePreprocess } from "@sveltejs/vite-plugin-svelte";

// Compile components as custom elements. Each component carries its tag via
// <svelte:options customElement="xi-..."/>; importing it auto-defines the element.
export default {
  preprocess: vitePreprocess(),
  compilerOptions: { customElement: true },
};
