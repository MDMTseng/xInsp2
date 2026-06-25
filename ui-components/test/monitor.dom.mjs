// monitor.dom.mjs — export mode A renderer (task #78): tiles bound to output keys,
// fed by the vars stream; collectStatusItems flattens status sections.
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom, tick } from "./_dom.mjs";

const w = setupDom();
await import("../dist/xi-components.esm.js");
const { mountMonitor, collectStatusItems } = await import("../src/monitor.mjs");

function mockClient() {
  const varsCbs = new Set(), prevCbs = new Set();
  return {
    onVars(cb) { varsCbs.add(cb); return () => varsCbs.delete(cb); },
    onPreview(cb) { prevCbs.add(cb); return () => prevCbs.delete(cb); },
    emitVars(items) { for (const cb of varsCbs) cb({ items }); },
    emitPreview(f) { for (const cb of prevCbs) cb(f); },
  };
}

test("collectStatusItems flattens only status sections", () => {
  const items = collectStatusItems([
    { section: "Live", tag: "status", controls: [
      { type: "value", key: "count", label: "Count" },
      { type: "trace", key: "rate" },
    ] },
    { section: "Tuning", tag: "control", controls: [{ type: "slider", key: "threshold" }] },
  ]);
  assert.deepEqual(items, [
    { type: "value", key: "count", label: "Count" },
    { type: "trace", key: "rate", label: "rate" },
  ], "status items only, control section ignored");
});

test("mountMonitor renders tiles and updates them from vars", async () => {
  const client = mockClient();
  const host = w.document.createElement("div");
  w.document.body.appendChild(host);

  const mon = mountMonitor(host, { client, items: [
    { type: "value", key: "count", label: "Count" },
    { type: "trace", key: "rate", label: "Rate" },
  ] });
  await tick(w);

  const tiles = host.querySelectorAll(".xi-tile");
  assert.equal(tiles.length, 2, "one tile per item");
  assert.ok(host.querySelector("xi-trace"), "trace tile rendered");

  client.emitVars({
    count: { name: "count", kind: "number", value: 42 },
    rate: { name: "rate", kind: "number", value: 3 },
  });
  await tick(w);

  const valueTile = [...tiles].find((t) => t.dataset.key === "count");
  assert.equal(valueTile.querySelector(".xi-value").textContent, "42", "value tile shows latest");
  assert.equal(host.querySelector("xi-trace").latest, 3, "trace tile fed from vars");

  mon.destroy();
  assert.equal(host.children.length, 0, "destroy tears down");
});
