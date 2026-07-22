// mount-schema.mjs — the UI half of the `controls` pluginlet (doc 37), OWNED BY
// THE PLET. Renders the $schema UI TREE that the native half
// (toolbox/pluginlets/controls/controls.hpp) emits from get_def, and wires every
// value control back through set_instance_def.
//
// SOURCE FORM, like the native half: the plet ships .mjs/.svelte sources and the
// CONSUMER'S build compiles them (native → the plugin's CMake via
// xi_use_pluginlet; ui → the webui's Svelte/vite build). A plet never ships a
// prebuilt artifact.
//
// DEPENDENCY DIRECTION (doc 37): a pluginlet is a LEAF — it must not import from
// the app layer (@xinsp2/components). This module is therefore self-contained: it
// owns its widget-tag map and element creation. The app (extension / HMI / core)
// depends on the PLET when it wants a schema panel, never the other way round.
//
// It deliberately does NOT fall back to the app's flat mountPanel: choosing the
// renderer is the caller's job (has $schema → this; otherwise → the app's panel).
//
// Widget vocabulary + $schema shape: ../contract.ts (the single source of truth).

// $schema widget -> the custom-element tag that renders it. stepper/file/color have
// no dedicated element yet and degrade to the nearest one; the intent rides along
// as data-widget so a future xi-stepper/xi-file/xi-color picks it up with NO schema
// change. `range` is handled separately (it binds TWO keys).
const WIDGET_TAG = {
  slider: "xi-slider",
  numpad: "xi-number",
  stepper: "xi-number",
  toggle: "xi-toggle",
  dropdown: "xi-dropdown",
  radio: "xi-radio",
  text: "xi-text",
  file: "xi-text",
  color: "xi-text",
};

// Create ONE bound control element. Attributes only — the caller appends it and
// then applies `value`/`options` (a Svelte custom element picks those up reliably
// once connected). `onChange(value)` fires per edit.
function makeControlEl(doc, tag, { label, min, max, step }, onChange) {
  const el = doc.createElement(tag);
  if (label) el.setAttribute("label", label);
  for (const [a, v] of [["min", min], ["max", max], ["step", step]])
    if (v != null) el.setAttribute(a, String(v));
  el.addEventListener("change", (e) => onChange(e.detail.value, el));
  return el;
}

/**
 * Render an instance's $schema tree into `host`.
 * opts: { client, instance } — client needs getInstanceDef / setInstanceDef and
 * (for buttons) optionally exchangeInstance.
 * Returns { refresh(), destroy() } or null when the instance declares no $schema.
 * Emits `xi-change` {key,value} on `host` per edit.
 */
export async function mountSchema(host, opts) {
  const { client, instance } = opts;
  const doc = host.ownerDocument || globalThis.document;
  const def = (await client.getInstanceDef(instance)) || {};
  const schema = def.$schema;
  if (!schema) return null;              // caller picks another renderer

  // Flat current values (everything but the reserved meta keys).
  const state = {};
  for (const [k, v] of Object.entries(def))
    if (k !== "$schema" && k !== "$v" && k !== "$rev") state[k] = v;

  const bound = [];  // {el, key, readout?} for refresh()

  const pushDef = async (key, value) => {
    state[key] = value;
    try { await client.setInstanceDef(instance, { ...state }); } catch { /* WS log */ }
    host.dispatchEvent(new CustomEvent("xi-change", { detail: { key, value }, bubbles: true }));
  };

  const applyBox = (el, node) => {
    if (node.span) el.style.gridColumn = `span ${node.span}`;
    if (node.rows) el.style.gridRow = `span ${node.rows}`;
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
        const el = makeControlEl(doc, "xi-number", {
          label: node.label ? `${node.label} ${suffix}` : k,
          min: node.min, max: node.max, step: node.step }, (value) => pushDef(k, value));
        wrap.appendChild(el);
        if (k in state) el.value = state[k];
        bound.push({ el, key: k });
      }
      applyBox(wrap, node); parent.appendChild(wrap); return;
    }
    // wired value control (slider/numpad/stepper/toggle/dropdown/radio/text/file/color)
    const tag = WIDGET_TAG[w] || "xi-number";
    const el = makeControlEl(doc, tag, { label: node.label, min: node.min,
      max: node.max, step: node.step }, (value) => pushDef(node.key, value));
    if (node.sem) el.setAttribute("data-sem", node.sem);   // semantic hint for styling/units
    // When the widget DEGRADES to another element (numpad/stepper→xi-number,
    // file/color→xi-text), keep the declared intent so a future dedicated element
    // can pick it up without a schema change.
    if (tag !== `xi-${w}`) el.setAttribute("data-widget", w);
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

export { WIDGET_TAG as CONTROLS_WIDGET_TAGS };
