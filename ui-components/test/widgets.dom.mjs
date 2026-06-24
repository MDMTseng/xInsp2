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
