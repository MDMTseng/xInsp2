//
// auto-panel.mjs — the "usable UI for free" renderer (task #76). Turns a
// control-descriptor (declared in a plugin's plugin.json manifest) into sections
// of xi-* widgets, each wired to set_instance_def. If no descriptor is given it
// INFERS one from the instance's current def (number→slider/number, boolean→
// toggle, string→text) — so even a plugin that declares nothing gets a basic
// panel. See docs/roadmap/webui-and-ui-export.md.
//
// Descriptor schema (array of sections):
//   [{ section: "Threshold", tag: "setup"|"control"|"status",
//      controls: [{ type, key, label?, min?, max?, step?, options? }] }]
//   type ∈ slider | number | toggle | radio | dropdown (unknown → number).
//

const TAG = {
  slider: "xi-slider",
  number: "xi-number",
  toggle: "xi-toggle",
  radio: "xi-radio",
  dropdown: "xi-dropdown",
};

// Build a one-section descriptor from a def object by guessing a control per key.
export function inferDescriptor(def, { section = "Config", tag = "control" } = {}) {
  const controls = [];
  for (const [key, v] of Object.entries(def || {})) {
    let type = "number";
    if (typeof v === "boolean") type = "toggle";
    else if (typeof v === "string") type = "text";
    else if (typeof v === "number") type = "number";
    else continue; // skip nested objects/arrays — a manifest should describe those
    controls.push({ type, key, label: key });
  }
  return controls.length ? [{ section, tag, controls }] : [];
}

// Render a descriptor into `host`, wiring each control to the instance over WS.
// opts: { client, instance, descriptor?, sectionFilter? }
// Returns { refresh(), destroy() }. Emits `xi-change` {key,value} on host per edit.
export async function mountPanel(host, opts) {
  const { client, instance, sectionFilter } = opts;
  const doc = host.ownerDocument || globalThis.document;
  const def = (await client.getInstanceDef(instance)) || {};
  const state = { ...def };
  const descriptor = opts.descriptor && opts.descriptor.length ? opts.descriptor : inferDescriptor(def);
  const controls = []; // {el, key} for refresh

  host.innerHTML = "";
  for (const sec of descriptor) {
    if (sectionFilter && !sectionFilter(sec)) continue;
    const secEl = doc.createElement("section");
    secEl.className = "xi-section";
    secEl.dataset.tag = sec.tag || "control";
    if (sec.section) {
      const h = doc.createElement("h3");
      h.className = "xi-section-title";
      h.textContent = sec.section;
      secEl.appendChild(h);
    }
    for (const ctl of sec.controls || []) {
      const tag = TAG[ctl.type] || "xi-number"; // total: unknown type degrades to number
      const el = doc.createElement(tag);
      if (ctl.label) el.setAttribute("label", ctl.label);
      for (const a of ["min", "max", "step"]) if (ctl[a] != null) el.setAttribute(a, String(ctl[a]));
      const wrap = doc.createElement("div");
      wrap.className = "xi-control";
      wrap.appendChild(el);
      secEl.appendChild(wrap);
      // Set properties AFTER the element is connected so the CE picks them up.
      if (ctl.options != null) el.options = ctl.options;
      if (ctl.key in state) el.value = state[ctl.key];
      el.addEventListener("change", async (e) => {
        state[ctl.key] = e.detail.value;
        try { await client.setInstanceDef(instance, { ...state }); } catch { /* surfaced via WS log */ }
        host.dispatchEvent(new CustomEvent("xi-change", { detail: { key: ctl.key, value: e.detail.value }, bubbles: true }));
      });
      controls.push({ el, key: ctl.key });
    }
    host.appendChild(secEl);
  }

  return {
    // Re-read the def from the backend and push values back into the controls.
    async refresh() {
      const d = (await client.getInstanceDef(instance)) || {};
      Object.assign(state, d);
      for (const { el, key } of controls) if (key in state) el.value = state[key];
    },
    destroy() { host.innerHTML = ""; },
  };
}

export { TAG as CONTROL_TAGS };
