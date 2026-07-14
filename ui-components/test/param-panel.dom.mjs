// param-panel.dom.mjs — the manifest param-panel (doc 31): a plugin manifest
// `params` block → a form of wired controls, each edit firing set_param (single
// parameter), distinct from mountPanel's whole-def set_instance_def.
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom, tick } from "./_dom.mjs";

const w = setupDom();
await import("../dist/xi-components.esm.js");     // registers xi-*
const { mountParamPanel, paramsToControls } = await import("../src/auto-panel.mjs");

function mockClient() {
  return { calls: [], async setParam(name, value) { this.calls.push({ name, value }); } };
}

test("paramsToControls infers a widget type per param", () => {
  assert.deepEqual(
    paramsToControls([
      { name: "sigma", min: 0, max: 10, default: 3 },        // min&max → slider
      { name: "mode", options: ["a", "b"] },                 // options → dropdown
      { name: "on", default: true },                         // boolean → toggle
      { name: "label", default: "hi" },                      // string → text
      { name: "count", default: 4 },                         // else → number
    ]).map((c) => [c.key, c.type]),
    [["sigma", "slider"], ["mode", "dropdown"], ["on", "toggle"], ["label", "text"], ["count", "number"]]);
});

test("renders a manifest params block and fires set_param per edit", async () => {
  const client = mockClient();
  const host = w.document.createElement("div");
  w.document.body.appendChild(host);

  const params = [
    { name: "sigma", type: "slider", min: 0, max: 10, default: 3 },
    { name: "invert", type: "toggle", default: false },
  ];
  const panel = mountParamPanel(host, { client, params });
  await tick(w);

  assert.equal(host.querySelector("section").dataset.tag, "param", "param section");
  assert.ok(host.querySelector("xi-slider"), "slider rendered");
  assert.equal(host.querySelector("xi-slider").value, 3, "seeded from default");
  assert.deepEqual(panel.values(), { sigma: 3, invert: false }, "defaults seed the state");

  host.querySelector("xi-slider").dispatchEvent(
    new w.CustomEvent("change", { detail: { value: 7 }, bubbles: true }));
  await tick(w);

  assert.deepEqual(client.calls, [{ name: "sigma", value: 7 }],
    "one set_param(name, value) — single parameter, not the whole def");
  assert.equal(panel.values().sigma, 7, "local state tracks the edit");
});
