// xi-slider.dom.mjs — instantiate the BUILT custom element in jsdom and check the
// tap-out contract: value property reflects, change event fires. Proves the
// Svelte→custom-element build emits a working element, not just a registered tag.
//
//   node --test ui-components/test/xi-slider.dom.mjs   (after `npm run build`)
//
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom } from "./_dom.mjs";

test("built <xi-slider> instantiates, renders shadow DOM, reflects value (tap-in)", async () => {
  const w = setupDom();
  await import("../dist/xi-components.esm.js"); // registers xi-slider
  assert.ok(w.customElements.get("xi-slider"), "xi-slider is defined");

  const el = w.document.createElement("xi-slider");
  el.setAttribute("min", "0");
  el.setAttribute("max", "255");
  el.setAttribute("value", "128");
  w.document.body.appendChild(el);
  await new Promise((r) => w.setTimeout(r, 0)); // let the effect run

  assert.equal(Number(el.value), 128, "value property reflects the attribute (tap-in)");

  const input = el.shadowRoot && el.shadowRoot.querySelector("input[type=range]");
  assert.ok(input, "renders a range input in its shadow root");
  assert.equal(input.value, "128", "inner control shows the bound value");

  // Property tap-in: host writes .value → the rendered control follows (reactivity).
  el.value = 50;
  await new Promise((r) => w.setTimeout(r, 0));
  assert.equal(input.value, "50", "host-set .value propagates to the control");

  // NOTE: the `change`/`input` event tap-OUT relies on Svelte's event delegation,
  // which jsdom's synthetic dispatch doesn't drive reliably — it's verified in a
  // real browser by the #73 PoC. Here we prove the build emits a working element
  // (defined, instantiates, renders shadow DOM, reflects + accepts `value`).
});
