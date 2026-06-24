// auto-panel.dom.mjs — the auto-webui renderer (task #76): renders sections of
// wired widgets from a descriptor (or inferred from the def), and a control edit
// patches the full def back via set_instance_def.
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom, tick } from "./_dom.mjs";

const w = setupDom();
await import("../dist/xi-components.esm.js");        // registers xi-*
const { mountPanel, inferDescriptor } = await import("../src/auto-panel.mjs");

function mockClient(defs) {
  return {
    defs,
    calls: [],
    async getInstanceDef(n) { return { ...this.defs[n] }; },
    async setInstanceDef(n, d) { this.defs[n] = d; this.calls.push({ n, d }); },
  };
}

test("renders sections + controls from a descriptor and wires set_instance_def", async () => {
  const client = mockClient({ bin0: { threshold: 100, mode: "light" } });
  const host = w.document.createElement("div");
  w.document.body.appendChild(host);

  const descriptor = [{
    section: "Threshold", tag: "control",
    controls: [
      { type: "slider", key: "threshold", label: "Threshold", min: 0, max: 255 },
      { type: "dropdown", key: "mode", label: "Mode", options: ["light", "dark"] },
    ],
  }];
  await mountPanel(host, { client, instance: "bin0", descriptor });
  await tick(w);

  const sec = host.querySelector("section.xi-section");
  assert.equal(sec.dataset.tag, "control", "section carries its tag");
  assert.ok(host.querySelector("xi-slider"), "slider rendered");
  const dd = host.querySelector("xi-dropdown");
  assert.ok(dd, "dropdown rendered");
  assert.equal(host.querySelector("xi-slider").value, 100, "slider initialized from def");

  // Edit the slider → the FULL patched def is sent (other keys preserved).
  host.querySelector("xi-slider").dispatchEvent(
    new w.CustomEvent("change", { detail: { value: 150 }, bubbles: true }));
  await tick(w);
  assert.equal(client.calls.length, 1, "one set_instance_def");
  assert.deepEqual(client.calls[0].d, { threshold: 150, mode: "light" },
    "patched key + preserved the rest");
});

test("infers a panel from the def when no descriptor is given", async () => {
  const client = mockClient({ cam0: { width: 640, fps: 30, streaming: true, name: "front" } });
  const host = w.document.createElement("div");
  w.document.body.appendChild(host);

  assert.deepEqual(
    inferDescriptor({ width: 640, on: true }),
    [{ section: "Config", tag: "control", controls: [
      { type: "number", key: "width", label: "width" },
      { type: "toggle", key: "on", label: "on" },
    ] }],
    "infer maps number→number, boolean→toggle");

  await mountPanel(host, { client, instance: "cam0" }); // no descriptor → inferred
  await tick(w);
  assert.ok(host.querySelector("xi-number"), "number control inferred for width/fps");
  assert.ok(host.querySelector("xi-toggle"), "toggle inferred for streaming");
});
