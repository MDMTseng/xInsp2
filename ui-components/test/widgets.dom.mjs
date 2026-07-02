// widgets.dom.mjs — jsdom smoke for the bindable control widgets (task #74):
// each built element instantiates, renders, and reflects its value/options.
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom, tick } from "./_dom.mjs";

const w = setupDom();
await import("../dist/xi-components.esm.js"); // registers all xi-*

async function mount(html) {
  w.document.body.innerHTML = html;
  const el = w.document.body.firstElementChild;
  await tick(w);
  return el;
}

test("xi-number reflects value and renders a number input", async () => {
  const el = await mount(`<xi-number value="7" min="0" max="10"></xi-number>`);
  assert.ok(w.customElements.get("xi-number"), "defined");
  assert.equal(Number(el.value), 7, "value reflects");
  assert.ok(el.shadowRoot.querySelector("input[type=number]"), "renders number input");
});

test("xi-toggle reflects boolean value", async () => {
  const el = await mount(`<xi-toggle value></xi-toggle>`);
  const box = el.shadowRoot.querySelector("input[type=checkbox]");
  assert.ok(box, "renders checkbox");
  assert.equal(box.checked, true, "checked follows value=true");
});

test("xi-dropdown renders options from a JSON attribute", async () => {
  const el = await mount(`<xi-dropdown value="b" options='["a","b","c"]'></xi-dropdown>`);
  const opts = [...el.shadowRoot.querySelectorAll("option")].map((o) => o.value);
  assert.deepEqual(opts, ["a", "b", "c"], "options parsed + rendered");
  assert.equal(el.value, "b", "value reflects");
});

test("xi-radio renders one input per option", async () => {
  const el = await mount(`<xi-radio value="2" options='[{"value":"1","label":"One"},{"value":"2","label":"Two"}]'></xi-radio>`);
  const radios = el.shadowRoot.querySelectorAll("input[type=radio]");
  assert.equal(radios.length, 2, "two radios");
  const checked = [...radios].find((r) => r.checked);
  assert.equal(checked && checked.value, "2", "selected reflects value");
});

test("xi-text exposes value and emits input", async () => {
  const el = await mount(`<xi-text value="hi" placeholder="p"></xi-text>`);
  assert.ok(w.customElements.get("xi-text"), "defined");
  assert.equal(el.value, "hi", "value reflects");
  const input = el.shadowRoot.querySelector("input[type=text]");
  assert.equal(input.placeholder, "p", "placeholder passthrough");
  let got = null;
  el.addEventListener("input", (e) => { got = e.detail.value; });
  input.value = "world";
  input.dispatchEvent(new w.window.Event("input", { bubbles: true }));
  await tick(w);
  assert.equal(got, "world", "input event carries new value");
});

test("xi-button renders a button and bubbles a composed click", async () => {
  const el = await mount(`<xi-button>Go</xi-button>`);
  assert.ok(w.customElements.get("xi-button"), "defined");
  const btn = el.shadowRoot.querySelector("button");
  assert.ok(btn, "renders inner button");
  let clicked = false;
  el.addEventListener("click", () => { clicked = true; });
  btn.dispatchEvent(new w.window.MouseEvent("click", { bubbles: true, composed: true }));
  assert.equal(clicked, true, "inner click retargets to host");
});

test("xi-badge projects host text through its slot", async () => {
  const el = await mount(`<xi-badge variant="counter">3 fields</xi-badge>`);
  assert.ok(w.customElements.get("xi-badge"), "defined");
  assert.ok(el.shadowRoot.querySelector("slot"), "has a slot");
  assert.equal(el.getAttribute("variant"), "counter", "variant reflects");
  assert.equal(el.textContent.trim(), "3 fields", "text lives in light DOM for the slot");
});
