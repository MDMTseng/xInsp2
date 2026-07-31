// mount-schema.test.mjs — the controls pluginlet's OWN UI test (the plet owns its
// UI half and its tests). Covers mount-schema.mjs: rendering the $schema TREE
// (tabs / grid / widgets incl. radio/readout/view/button/range) and wiring value
// edits back through set_instance_def, plus the app-layer chooser's fallback.
// Pure-Node: a tiny fake DOM (no jsdom, no built Svelte bundle) so it verifies the
// tree/wiring LOGIC without the frontend toolchain. (Real xi-* widget rendering
// needs the webui build — `npm run build` in ui-components.)
import { test } from "node:test";
import assert from "node:assert/strict";

// ---- minimal fake DOM ------------------------------------------------------
class ClassList {
  constructor() { this._s = new Set(); }
  add(c) { this._s.add(c); }
  toggle(c, b) { if (b === undefined) b = !this._s.has(c); b ? this._s.add(c) : this._s.delete(c); }
  contains(c) { return this._s.has(c); }
}
class El {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.children = []; this.attributes = {};
    this.style = {}; this.dataset = {}; this._l = {}; this.classList = new ClassList();
    this.ownerDocument = doc;
  }
  setAttribute(n, v) { this.attributes[n] = String(v); }
  getAttribute(n) { return this.attributes[n]; }
  appendChild(c) { this.children.push(c); c.parentNode = this; return c; }
  insertBefore(node, ref) {
    const i = this.children.indexOf(ref);
    if (i < 0) this.children.unshift(node); else this.children.splice(i, 0, node);
    node.parentNode = this; return node;
  }
  get firstChild() { return this.children[0]; }
  set innerHTML(v) { if (v === "") this.children = []; }
  addEventListener(t, fn) { (this._l[t] = this._l[t] || []).push(fn); }
  dispatchEvent(ev) { (this._l[ev.type] || []).forEach((fn) => fn(ev)); return true; }
}
const doc = { createElement: (t) => new El(t) };
globalThis.document = doc;
globalThis.CustomEvent = class { constructor(type, o = {}) { this.type = type; this.detail = o.detail; this.bubbles = o.bubbles; } };

const all = (el) => { const o = []; (function rec(n) { for (const c of n.children || []) { o.push(c); rec(c); } })(el); return o; };
const byTag = (el, t) => all(el).filter((e) => e.tagName === t.toUpperCase());
const byClass = (el, c) => all(el).filter((e) => (e.className || "").split(" ").includes(c));

const { mountSchema, registerWidget, unregisterWidget } = await import("./mount-schema.mjs");
const { mountInstancePanel } = await import("../../../../ui-components/src/auto-panel.mjs");

function schemaDef() {
  const schema = { type: "root", children: [
    { type: "tab", title: "Capture", children: [
      { type: "grid", columns: 12, children: [
        { type: "control", widget: "slider", key: "fps", label: "Frame rate", min: 1, max: 60, span: 6 },
        { type: "control", widget: "numpad", key: "gain", label: "Gain", min: 0.1, max: 4, span: 6 },
        { type: "control", widget: "toggle", key: "streaming", label: "Streaming", span: 6 },
        { type: "control", widget: "dropdown", key: "mode", label: "Mode", options: ["fast", "accurate"], span: 6 },
        { type: "control", widget: "radio", key: "trigger", label: "Trigger", options: ["soft", "hardware", "free"], span: 12 },
      ] },
      { type: "control", widget: "divider" },
      { type: "control", widget: "view", channel: "ui/cam0/preview", label: "Live", span: 12, rows: 6 },
    ] },
    { type: "tab", title: "Output", children: [
      { type: "control", widget: "readout", key: "ticks", label: "Ticks", span: 6 },
      { type: "control", widget: "button", command: "reset", label: "Reset", span: 6 },
    ] },
  ] };
  return { fps: 30, gain: 1.6, streaming: true, mode: "accurate", trigger: "hardware",
           ticks: "7", $v: 1, $rev: 1, $schema: schema };
}
function mockClient() {
  return {
    def: schemaDef(), calls: [], ex: [],
    async getInstanceDef() { return { ...this.def }; },
    async setInstanceDef(n, d) { this.calls.push(d); },
    exchangeInstance(n, c) { this.ex.push({ n, c }); },
  };
}

test("mountSchema renders the tab/grid tree with the right xi-* widgets", async () => {
  const client = mockClient();
  const host = doc.createElement("div"); host.ownerDocument = doc;
  await mountSchema(host, { client, instance: "cd" });

  // tabs: one tabbar, two tab buttons
  assert.equal(byClass(host, "xi-tabbar").length, 1, "a tab bar");
  assert.equal(byClass(host, "xi-tab").length, 2, "two tab buttons (Capture, Output)");

  // grid with 12 columns
  const grid = byClass(host, "xi-grid")[0];
  assert.ok(grid, "a grid container");
  assert.equal(grid.style.gridTemplateColumns, "repeat(12, 1fr)", "grid columns applied");

  // slider seeded from def, min/max carried
  const slider = byTag(host, "xi-slider")[0];
  assert.ok(slider, "slider rendered");
  assert.equal(slider.attributes.min, "1"); assert.equal(slider.attributes.max, "60");
  assert.equal(slider.value, 30, "slider seeded from def");

  // widget vocabulary mapped
  assert.equal(byTag(host, "xi-number").length, 1, "numpad → xi-number");
  assert.equal(byTag(host, "xi-toggle").length, 1, "toggle");
  assert.equal(byTag(host, "xi-dropdown").length, 1, "dropdown");
  const radio = byTag(host, "xi-radio")[0];
  assert.deepEqual(radio.options, ["soft", "hardware", "free"], "radio carries its options");

  // span applied to the control wrapper
  const wrap = slider.parentNode;
  assert.equal(wrap.style.gridColumn, "span 6", "span → grid-column");

  // presentation + readout + view + button
  assert.equal(byClass(host, "xi-divider").length, 1, "divider");
  const readoutV = byClass(host, "xi-readout-v")[0];
  assert.equal(readoutV.textContent, "7", "readout shows the plugin-pushed value");
  const view = byClass(host, "xi-view")[0];
  assert.equal(view.dataset.channel, "ui/cam0/preview", "view names its live-view channel");
  assert.equal(view.style.gridColumn, "span 12"); assert.equal(view.style.gridRow, "span 6");
  assert.equal(byClass(host, "xi-button").length, 1, "button");
});

test("editing a control patches the full def via set_instance_def", async () => {
  const client = mockClient();
  const host = doc.createElement("div"); host.ownerDocument = doc;
  await mountSchema(host, { client, instance: "cd" });

  const slider = byTag(host, "xi-slider")[0];
  slider.dispatchEvent(new CustomEvent("change", { detail: { value: 45 } }));
  await new Promise((r) => setTimeout(r, 0));   // let the async pushDef settle

  assert.equal(client.calls.length, 1, "one set_instance_def");
  const sent = client.calls[0];
  assert.equal(sent.fps, 45, "edited key patched");
  assert.equal(sent.mode, "accurate", "other keys preserved");
  assert.equal(sent.trigger, "hardware", "other keys preserved");
});

test("a button fires an exchange command", async () => {
  const client = mockClient();
  const host = doc.createElement("div"); host.ownerDocument = doc;
  await mountSchema(host, { client, instance: "cd" });
  byClass(host, "xi-button")[0].dispatchEvent(new CustomEvent("click", {}));
  assert.deepEqual(client.ex, [{ n: "cd", c: { command: "reset" } }], "button → exchangeInstance(reset)");
});

test("extended widgets: stepper/file/color degrade + carry sem; range → two controls", async () => {
  const $schema = { type: "root", children: [
    { type: "control", widget: "stepper", key: "count", label: "Count", min: 0, max: 100, step: 1, sem: "count" },
    { type: "control", widget: "file", key: "model", label: "Model" },
    { type: "control", widget: "color", key: "tint", label: "Tint" },
    { type: "control", widget: "range", key: "low", key2: "high", label: "Band", min: 0, max: 255, sem: "threshold" },
    { type: "control", widget: "slider", key: "thr", label: "Threshold", min: 0, max: 255, sem: "threshold" },
  ] };
  const client = {
    def: { count: 5, model: "net.onnx", tint: "#ff8800", low: 40, high: 200, thr: 128, $schema },
    calls: [],
    async getInstanceDef() { return { ...this.def }; },
    async setInstanceDef(n, d) { this.calls.push(d); },
  };
  const host = doc.createElement("div"); host.ownerDocument = doc;
  await mountSchema(host, { client, instance: "cd" });

  // every widget now has its OWN element (no degrade except numpad)
  const stepper = byTag(host, "xi-stepper")[0];
  assert.ok(stepper, "stepper has a dedicated xi-stepper element");
  assert.equal(stepper.attributes.step, "1", "stepper step carried");
  assert.equal(stepper.attributes["data-sem"], "count", "sem carried on the element");
  assert.equal(byTag(host, "xi-file").length, 1, "file → xi-file");
  assert.equal(byTag(host, "xi-color").length, 1, "color → xi-color");
  assert.ok(byTag(host, "xi-slider").some((e) => e.attributes["data-sem"] === "threshold"), "slider carries sem");

  // range → ONE xi-range control bound to both keys, both ends seeded
  const band = byTag(host, "xi-range")[0];
  assert.ok(band, "range has a dedicated dual-handle element");
  assert.equal(band.attributes["data-sem"], "threshold");
  assert.equal(band.attributes.min, "0"); assert.equal(band.attributes.max, "255");
  assert.equal(band.low, 40); assert.equal(band.high, 200, "both ends seeded");

  // one drag emits {low,high} and patches BOTH keys in a single def write
  band.dispatchEvent(new CustomEvent("change", { detail: { low: 60, high: 180 } }));
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(client.calls.at(-1).high, 180, "band high patched");
  assert.equal(client.calls.at(-1).low, 60, "band low patched in the same write");
});

test("registerWidget: a factory extends the vocabulary and joins refresh/destroy", async () => {
  // A "chart plet" claims the widget name `chart` — controls never heard of it.
  const seen = { nodes: [], updates: [], destroyed: 0 };
  registerWidget("chart", (node, ctx) => {
    seen.nodes.push(node);
    assert.equal(typeof ctx.pushDef, "function", "ctx exposes the def write path");
    assert.equal(ctx.instance, "cd");
    const el = ctx.doc.createElement("xi-chart");
    return {
      el,
      update: (state) => seen.updates.push(state.series),
      destroy: () => seen.destroyed++,
    };
  });
  try {
    const $schema = { type: "root", children: [
      { type: "control", widget: "chart", key: "series", label: "Trend", span: 12 },
    ] };
    const client = {
      def: { series: "a", $schema },
      async getInstanceDef() { return { ...this.def }; },
      async setInstanceDef() {},
    };
    const host = doc.createElement("div"); host.ownerDocument = doc;
    const panel = await mountSchema(host, { client, instance: "cd" });

    assert.equal(byTag(host, "xi-chart").length, 1, "factory's element mounted");
    assert.equal(byTag(host, "xi-chart")[0].style.gridColumn, "span 12", "span applied to it");
    assert.equal(seen.nodes[0].key, "series", "factory sees its schema node");

    client.def.series = "b";
    await panel.refresh();
    assert.deepEqual(seen.updates, ["b"], "update() joins refresh with fresh state");
    panel.destroy();
    assert.equal(seen.destroyed, 1, "destroy() joins the panel's destroy");
  } finally { unregisterWidget("chart"); }
});

test("registerWidget: overriding `view` mounts a real widget in the layout slot", async () => {
  // The live-view wiring: ONE explicit line in the webui replaces the empty
  // .xi-view placeholder with an actual viewer, channel defaulting to ui/<instance>.
  registerWidget("view", (node, { doc, instance }) => {
    const el = doc.createElement("div");
    el.className = "xi-live";
    el.dataset.channel = node.channel || `ui/${instance}`;
    return el;                                   // bare-element form
  });
  try {
    const client = mockClient();
    const host = doc.createElement("div"); host.ownerDocument = doc;
    await mountSchema(host, { client, instance: "cd" });
    assert.equal(byClass(host, "xi-view").length, 0, "built-in placeholder replaced");
    assert.equal(byClass(host, "xi-live")[0].dataset.channel, "ui/cam0/preview",
      "schema-declared channel wins");
  } finally { unregisterWidget("view"); }

  // built-in behaviour restored after unregister
  const host2 = doc.createElement("div"); host2.ownerDocument = doc;
  await mountSchema(host2, { client: mockClient(), instance: "cd" });
  assert.equal(byClass(host2, "xi-view").length, 1, "unregister restores the built-in");
});

test("registerWidget: tag-string form and per-mount `widgets` override", async () => {
  registerWidget("gauge", "xi-gauge");           // tag form: a bound value control
  try {
    const $schema = { type: "root", children: [
      { type: "control", widget: "gauge", key: "pressure", min: 0, max: 10 },
      { type: "control", widget: "slider", key: "thr", min: 0, max: 255 },
    ] };
    const client = {
      def: { pressure: 4, thr: 128, $schema }, calls: [],
      async getInstanceDef() { return { ...this.def }; },
      async setInstanceDef(n, d) { this.calls.push(d); },
    };
    const host = doc.createElement("div"); host.ownerDocument = doc;
    await mountSchema(host, { client, instance: "cd" });
    const g = byTag(host, "xi-gauge")[0];
    assert.ok(g, "tag-string impl rendered");
    assert.equal(g.value, 4, "seeded from def like any bound control");
    g.dispatchEvent(new CustomEvent("change", { detail: { value: 7 } }));
    await new Promise((r) => setTimeout(r, 0));
    assert.equal(client.calls.at(-1).pressure, 7, "edits flow through set_instance_def");

    // per-mount widgets beats the global registry (here: replace the built-in slider)
    const host2 = doc.createElement("div"); host2.ownerDocument = doc;
    await mountSchema(host2, { client, instance: "cd",
      widgets: { slider: "xi-dial" } });
    assert.equal(byTag(host2, "xi-dial").length, 1, "per-mount override applied");
    assert.equal(byTag(host2, "xi-slider").length, 0, "built-in tag not used");
  } finally { unregisterWidget("gauge"); }
});

test("an unknown widget renders an info placeholder naming the missing wiring", async () => {
  const $schema = { type: "root", children: [
    { type: "control", widget: "histogram", key: "bins", label: "Histogram", span: 12 },
    { type: "control", widget: "slider", key: "thr", min: 0, max: 255 },
  ] };
  const client = {
    def: { bins: "[]", thr: 128, $schema },
    async getInstanceDef() { return { ...this.def }; },
    async setInstanceDef() {},
  };
  const host = doc.createElement("div"); host.ownerDocument = doc;
  await mountSchema(host, { client, instance: "cd" });

  const ph = byClass(host, "xi-missing")[0];
  assert.ok(ph, "placeholder rendered instead of a guessed control");
  assert.ok(ph.textContent.includes('"histogram"'), "names the missing widget");
  assert.ok(ph.textContent.includes("bins"), "names the orphaned key");
  assert.ok(ph.textContent.includes('registerWidget("histogram"'), "tells the user the exact wiring line");
  assert.equal(ph.style.gridColumn, "span 12", "keeps its layout slot");
  assert.equal(byTag(host, "xi-slider").length, 1, "known widgets unaffected");

  // registering it afterwards resolves the placeholder on the next mount
  registerWidget("histogram", (node, { doc: d }) => d.createElement("xi-hist"));
  try {
    const host2 = doc.createElement("div"); host2.ownerDocument = doc;
    await mountSchema(host2, { client, instance: "cd" });
    assert.equal(byClass(host2, "xi-missing").length, 0, "no placeholder once wired");
    assert.equal(byTag(host2, "xi-hist").length, 1, "the registered component mounts");
  } finally { unregisterWidget("histogram"); }
});

test("the plet renderer returns null when there is no $schema (it never falls back)", async () => {
  const client = {
    def: { width: 640 },
    async getInstanceDef() { return { ...this.def }; },
    async setInstanceDef() {},
  };
  const host = doc.createElement("div"); host.ownerDocument = doc;
  assert.equal(await mountSchema(host, { client, instance: "x" }), null,
    "choosing another renderer is the caller's job, not the plet's");
});

test("the APP chooser falls back to the flat mountPanel path", async () => {
  const client = {
    def: { width: 640, streaming: true },
    calls: [],
    async getInstanceDef() { return { ...this.def }; },
    async setInstanceDef(n, d) { this.calls.push(d); },
  };
  const host = doc.createElement("div"); host.ownerDocument = doc;
  const r = await mountInstancePanel(host, { client, instance: "x" });
  assert.ok(r && typeof r.destroy === "function", "returns a panel handle");
  assert.ok(byTag(host, "xi-number").length >= 1, "inferred a number control (fallback)");
  assert.ok(byTag(host, "xi-toggle").length >= 1, "inferred a toggle (fallback)");
});
