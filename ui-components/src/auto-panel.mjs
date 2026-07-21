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

// ---- controls pluginlet $schema renderer (docs/new_gen/37) ------------------
// The controls pluginlet's native half (xi::pluginlet::Controls) emits, from
// get_def, a richer UI TREE than the flat descriptor above: containers (tab /
// section / grid) holding widget leaves, with per-node span/rows/caption and a
// `view` leaf that names a live-view channel. mountSchema renders that tree with
// the SAME xi-* widgets and wires value controls to set_instance_def exactly like
// mountPanel — so a plugin that declares a $schema (controls_demo) gets the full
// tabbed/grid panel for free, and everything degrades to the flat path when no
// $schema is present.
//
// Widget vocabulary -> control type (doc 37 contract.ts): slider, numpad(→number),
// toggle, dropdown, radio, text are wired value controls; button fires an
// exchange command; readout is a plugin-pushed read-only value; view is a
// live-view mount slot; title/label/divider are presentation-only.

// Map a $schema widget to an existing xi-* control type. stepper/file/color have
// no dedicated xi-* widget yet, so they degrade to the nearest one (number/text)
// — the semantic (`sem`) + step ride along so a future xi-stepper/xi-file/xi-color
// can pick them up without any schema change.
const SCHEMA_WIDGET_TYPE = {
  slider: "slider", numpad: "number", stepper: "number", toggle: "toggle",
  dropdown: "dropdown", radio: "radio", text: "text", file: "text", color: "text",
};

// opts: { client, instance }. Reads the instance def (flat values + $schema),
// renders the tree, wires edits back via set_instance_def. Returns
// { refresh(), destroy() }. Falls back to mountPanel when there is no $schema.
export async function mountSchema(host, opts) {
  const { client, instance } = opts;
  const doc = host.ownerDocument || globalThis.document;
  const def = (await client.getInstanceDef(instance)) || {};
  const schema = def.$schema;
  if (!schema) return mountPanel(host, opts);          // no tree → flat/inferred panel

  // Flat current values (everything but the reserved meta keys).
  const state = {};
  for (const [k, v] of Object.entries(def))
    if (k !== "$schema" && k !== "$v" && k !== "$rev") state[k] = v;

  const bound = [];  // {el, key} value controls, for refresh()

  const pushDef = async (key, value) => {
    state[key] = value;
    try { await client.setInstanceDef(instance, { ...state }); } catch { /* WS log */ }
    host.dispatchEvent(new CustomEvent("xi-change", { detail: { key, value }, bubbles: true }));
  };

  // Render one node into `parent`. Containers recurse; leaves render a widget.
  const renderNode = (node, parent) => {
    if (!node) return;
    if (node.type === "control") return renderLeaf(node, parent);
    if (node.type === "tabs" || (node.children && node.children.some((c) => c.type === "tab")))
      return renderTabs(node, parent);
    if (node.type === "grid") return renderGrid(node, parent);
    if (node.type === "section") return renderSection(node, parent);
    // root / row / group / unknown container: transparent pass-through
    for (const c of node.children || []) renderNode(c, parent);
  };

  const applyBox = (el, node) => {
    if (node.span) el.style.gridColumn = `span ${node.span}`;
    if (node.rows) el.style.gridRow = `span ${node.rows}`;
  };

  const renderTabs = (node, parent) => {
    const tabs = (node.children || []).filter((c) => c.type === "tab");
    const wrap = doc.createElement("div"); wrap.className = "xi-tabs";
    const bar = doc.createElement("div"); bar.className = "xi-tabbar";
    const panels = [];
    tabs.forEach((t, i) => {
      const btn = doc.createElement("button");
      btn.className = "xi-tab"; btn.type = "button";
      btn.textContent = t.title || `Tab ${i + 1}`;
      const panel = doc.createElement("div"); panel.className = "xi-tabpanel";
      panel.style.display = i === 0 ? "" : "none";
      if (i === 0) btn.classList.add("active");
      btn.addEventListener("click", () => {
        panels.forEach((p, j) => { p.panel.style.display = j === i ? "" : "none";
          p.btn.classList.toggle("active", j === i); });
      });
      for (const c of t.children || []) renderNode(c, panel);
      bar.appendChild(btn); wrap.appendChild(panel); panels.push({ btn, panel });
    });
    wrap.insertBefore(bar, wrap.firstChild);
    parent.appendChild(wrap);
  };

  const renderGrid = (node, parent) => {
    const g = doc.createElement("div"); g.className = "xi-grid";
    g.style.display = "grid";
    g.style.gridTemplateColumns = `repeat(${node.columns || 12}, 1fr)`;
    for (const c of node.children || []) renderNode(c, g);
    parent.appendChild(g);
  };

  const renderSection = (node, parent) => {
    const s = doc.createElement("section"); s.className = "xi-section";
    if (node.collapsed) s.dataset.collapsed = "1";
    if (node.title) {
      const h = doc.createElement("h3"); h.className = "xi-section-title";
      h.textContent = node.title; s.appendChild(h);
    }
    const body = doc.createElement("div"); body.className = "xi-section-body";
    for (const c of node.children || []) renderNode(c, body);
    s.appendChild(body); applyBox(s, node); parent.appendChild(s);
  };

  const renderLeaf = (node, parent) => {
    const w = node.widget;
    // presentation-only
    if (w === "title" || w === "label") {
      const e = doc.createElement(w === "title" ? "h4" : "p");
      e.className = w === "title" ? "xi-title" : "xi-label";
      e.textContent = node.label || ""; applyBox(e, node); parent.appendChild(e); return;
    }
    if (w === "divider") { const e = doc.createElement("hr"); e.className = "xi-divider"; parent.appendChild(e); return; }
    if (w === "readout") {
      const e = doc.createElement("div"); e.className = "xi-readout";
      const k = doc.createElement("div"); k.className = "xi-readout-k"; k.textContent = node.label || node.key || "";
      const v = doc.createElement("div"); v.className = "xi-readout-v"; v.textContent = String(state[node.key] ?? "");
      e.appendChild(k); e.appendChild(v); applyBox(e, node);
      if (node.key) bound.push({ el: v, key: node.key, readout: true });
      parent.appendChild(e); return;
    }
    if (w === "view") {
      const e = doc.createElement("div"); e.className = "xi-view";
      if (node.channel) e.dataset.channel = node.channel;   // where a live-view mounts
      if (node.label) e.setAttribute("label", node.label);
      applyBox(e, node); parent.appendChild(e); return;
    }
    if (w === "button") {
      const b = doc.createElement("button"); b.className = "xi-button"; b.type = "button";
      b.textContent = node.label || node.command || "";
      b.addEventListener("click", () => {
        if (client.exchangeInstance) client.exchangeInstance(instance, { command: node.command });
      });
      applyBox(b, node); parent.appendChild(b); return;
    }
    // range: one control bound to TWO keys (key = low, key2 = high) — until a
    // dedicated xi-range exists, render two wired numeric controls.
    if (w === "range") {
      const wrap = doc.createElement("div"); wrap.className = "xi-range";
      if (node.sem) wrap.dataset.sem = node.sem;
      for (const [k, suffix] of [[node.key, "min"], [node.key2, "max"]]) {
        if (!k) continue;
        const el = makeControlEl(doc, { type: "number", key: k,
          label: node.label ? `${node.label} ${suffix}` : k, min: node.min, max: node.max, step: node.step },
          (value) => pushDef(k, value));
        wrap.appendChild(el);
        if (k in state) el.value = state[k];
        bound.push({ el, key: k });
      }
      applyBox(wrap, node); parent.appendChild(wrap); return;
    }
    // wired value control (slider/numpad/stepper/toggle/dropdown/radio/text/file/color)
    const type = SCHEMA_WIDGET_TYPE[w] || "number";
    const el = makeControlEl(doc, { type, key: node.key, label: node.label,
      min: node.min, max: node.max, step: node.step }, (value) => pushDef(node.key, value));
    if (node.sem) el.setAttribute("data-sem", node.sem);        // semantic hint for styling/units
    if (w !== SCHEMA_WIDGET_TYPE[w]) el.setAttribute("data-widget", w);  // preserve intent (stepper/file/color)
    const wrap = doc.createElement("div"); wrap.className = "xi-control";
    wrap.appendChild(el); applyBox(wrap, node); parent.appendChild(wrap);
    if (node.options != null) el.options = node.options;
    if (node.key in state) el.value = state[node.key];
    bound.push({ el, key: node.key });
  };

  host.innerHTML = "";
  renderNode(schema, host);

  return {
    async refresh() {
      const d = (await client.getInstanceDef(instance)) || {};
      for (const [k, v] of Object.entries(d))
        if (k !== "$schema" && k !== "$v" && k !== "$rev") state[k] = v;
      for (const b of bound) {
        if (!(b.key in state)) continue;
        if (b.readout) b.el.textContent = String(state[b.key]);
        else b.el.value = state[b.key];
      }
    },
    destroy() { host.innerHTML = ""; },
  };
}
