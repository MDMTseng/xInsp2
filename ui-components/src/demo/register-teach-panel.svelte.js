// register-teach-panel.svelte.js — the REFERENCE registration for an app-custom
// widget built as a plain Svelte component tree (doc 37 registry, path ②):
// factory → svelte mount()/unmount(), with a $state object bridging the panel's
// refresh() into Svelte reactivity. `.svelte.js` so runes work in this module.
//
// Native side declares the slot with:  ctl_.comp("teach_panel", "roi").span(12)
// (keyed comp = the def slot the panel edits; declared → set_def validates it).
import { mount, unmount } from "svelte";
import TeachPanel from "./TeachPanel.svelte";
import { registerWidget } from "../../../toolbox/pluginlets/controls/ui/mount-schema.mjs";

export function registerTeachPanelDemo() {
  registerWidget("teach_panel", (node, { instance, state, pushDef }) => {
    const el = document.createElement("div");
    const props = $state({ values: { ...state }, pushDef, node, instance });
    const app = mount(TeachPanel, { target: el, props });
    return {
      el,
      update: (s) => { props.values = { ...s }; },   // refresh → runes take it from here
      destroy: () => unmount(app),
    };
  });
}
