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

const { mountSchema } = await import("./mount-schema.mjs");
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

  // stepper/file/color degrade to number/text but preserve intent + sem
  const stepper = byTag(host, "xi-number").find((e) => e.attributes["data-widget"] === "stepper");
  assert.ok(stepper, "stepper degraded to xi-number, data-widget preserved");
  assert.equal(stepper.attributes.step, "1", "stepper step carried");
  assert.equal(stepper.attributes["data-sem"], "count", "sem carried on the element");
  assert.ok(byTag(host, "xi-text").some((e) => e.attributes["data-widget"] === "file"), "file → xi-text");
  assert.ok(byTag(host, "xi-text").some((e) => e.attributes["data-widget"] === "color"), "color → xi-text");
  assert.ok(byTag(host, "xi-slider").some((e) => e.attributes["data-sem"] === "threshold"), "slider carries sem");

  // range → two numeric controls, one per key, both seeded
  const band = byClass(host, "xi-range")[0];
  assert.ok(band, "range wrapper");
  assert.equal(band.dataset.sem, "threshold");
  const nums = band.children;
  assert.equal(nums.length, 2, "two controls for the band");
  assert.equal(nums[0].value, 40); assert.equal(nums[1].value, 200);

  // editing the high handle patches only its key
  nums[1].dispatchEvent(new CustomEvent("change", { detail: { value: 180 } }));
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(client.calls.at(-1).high, 180, "band high patched");
  assert.equal(client.calls.at(-1).low, 40, "band low preserved");
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
