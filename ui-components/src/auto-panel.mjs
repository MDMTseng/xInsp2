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
  text: "xi-text",
};

// Create ONE control element from a control descriptor {type,key,label,min,max,
// step} and attach its change handler. Attributes only — the CALLER appends it
// and then applies `value`/`options` (a Svelte CE picks up those props reliably
// once connected). `onChange(value, el)` fires per edit. Shared by mountPanel
// (def → set_instance_def) and mountParamPanel (manifest → set_param).
function makeControlEl(doc, ctl, onChange) {
  const tag = TAG[ctl.type] || "xi-number";     // unknown type degrades to number
  const el = doc.createElement(tag);
  if (ctl.label) el.setAttribute("label", ctl.label);
  for (const a of ["min", "max", "step"]) if (ctl[a] != null) el.setAttribute(a, String(ctl[a]));
  el.addEventListener("change", (e) => onChange(e.detail.value, el));
  return el;
}

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
      const el = makeControlEl(doc, ctl, async (value) => {
        state[ctl.key] = value;
        try { await client.setInstanceDef(instance, { ...state }); } catch { /* surfaced via WS log */ }
        host.dispatchEvent(new CustomEvent("xi-change", { detail: { key: ctl.key, value }, bubbles: true }));
      });
      const wrap = doc.createElement("div");
      wrap.className = "xi-control";
      wrap.appendChild(el);
      secEl.appendChild(wrap);
      // Set properties AFTER the element is connected so the CE picks them up.
      if (ctl.options != null) el.options = ctl.options;
      if (ctl.key in state) el.value = state[ctl.key];
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

// ---- manifest param-panel (doc 31: set_param single-parameter set) ----------
// A plugin manifest's `params` block is a flat list of tunables:
//   [{ name, type?, min?, max?, step?, default?, options?, label? }]
// `type` ∈ slider|number|toggle|radio|dropdown|text; when omitted it is inferred
// from `default`/`options` (options → dropdown, boolean → toggle, min&max →
// slider, string → text, else number).
export function paramsToControls(params = []) {
  return (params || []).map((p) => {
    let type = p.type;
    if (!type) {
      if (Array.isArray(p.options)) type = "dropdown";
      else if (typeof p.default === "boolean") type = "toggle";
      else if (p.min != null && p.max != null) type = "slider";
      else if (typeof p.default === "string") type = "text";
      else type = "number";
    }
    return { type, key: p.name, label: p.label || p.name,
             min: p.min, max: p.max, step: p.step, options: p.options, default: p.default };
  });
}

// Render a manifest `params` block into a form of wired controls; each edit fires
// client.setParam(name, value) (doc 31 single-parameter set — NOT set_instance_def).
// opts: { client, params, section?, values? }  (values seeds initial control state)
// Returns { setValues(map), values(), destroy() }; emits `xi-param` {name,value}.
export function mountParamPanel(host, opts) {
  const { client, params, section = "Parameters", values = {} } = opts;
  const doc = host.ownerDocument || globalThis.document;
  const controls = paramsToControls(params);
  const state = { ...values };
  for (const c of controls) if (!(c.key in state) && c.default !== undefined) state[c.key] = c.default;
  const els = [];

  host.innerHTML = "";
  const secEl = doc.createElement("section");
  secEl.className = "xi-section"; secEl.dataset.tag = "param";
  if (section) {
    const h = doc.createElement("h3"); h.className = "xi-section-title"; h.textContent = section;
    secEl.appendChild(h);
  }
  for (const ctl of controls) {
    const el = makeControlEl(doc, ctl, async (value) => {
      state[ctl.key] = value;
      try { await client.setParam(ctl.key, value); } catch { /* rsp error surfaced via WS log */ }
      host.dispatchEvent(new CustomEvent("xi-param", { detail: { name: ctl.key, value }, bubbles: true }));
    });
    const wrap = doc.createElement("div"); wrap.className = "xi-control";
    wrap.appendChild(el); secEl.appendChild(wrap);
    if (ctl.options != null) el.options = ctl.options;
    if (ctl.key in state) el.value = state[ctl.key];
    els.push({ el, key: ctl.key });
  }
  host.appendChild(secEl);

  return {
    setValues(map) { Object.assign(state, map); for (const { el, key } of els) if (key in state) el.value = state[key]; },
    values() { return { ...state }; },
    destroy() { host.innerHTML = ""; },
  };
}

export { TAG as CONTROL_TAGS };

// ---- controls pluginlet: the $schema renderer lives IN THE PLET ---------------
// Dependency direction (doc 37): a pluginlet is a LEAF that owns its own UI; the
// app layer (this library, the extension, the HMI) DEPENDS ON the plet when it
// wants a schema panel — never the reverse. So mountSchema is imported from
// toolbox/pluginlets/controls/ui/ and only re-exported here for convenience.
export { mountSchema } from "../../toolbox/pluginlets/controls/ui/mount-schema.mjs";
import { mountSchema as _mountSchema } from "../../toolbox/pluginlets/controls/ui/mount-schema.mjs";

// mountInstancePanel(host, {client, instance, descriptor?}) — the APP-LEVEL
// chooser: an instance that declares a $schema gets the controls plet's tree
// renderer; anything else falls back to the flat descriptor/inferred panel. The
// plet itself never falls back (that choice is not a plet's business).
export async function mountInstancePanel(host, opts) {
  const panel = await _mountSchema(host, opts);
  return panel || mountPanel(host, opts);
}
