// xi-trace.dom.mjs — the key-trace component (task #75): update(items) picks its
// key, tracks latest + numeric history, emits `sample`. (Sparkline drawing is
// canvas/browser-only; here we verify the data path.)
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom, tick } from "./_dom.mjs";

const w = setupDom();
await import("../dist/xi-components.esm.js");

test("xi-trace tracks the latest value + numeric history for its key", async () => {
  w.document.body.innerHTML = `<xi-trace key="count" label="Count" max="3"></xi-trace>`;
  const el = w.document.body.firstElementChild;
  await tick(w);
  assert.ok(w.customElements.get("xi-trace"), "defined");

  let samples = 0;
  el.addEventListener("sample", () => samples++);

  el.update({ count: { name: "count", kind: "number", value: 5 } });
  el.update({ count: { name: "count", kind: "number", value: 8 } });
  el.update({ other: { name: "other", value: 99 } });   // not our key → ignored
  await tick(w);

  assert.equal(el.latest, 8, "latest follows our key");
  assert.deepEqual(el.history, [5, 8], "numeric history accumulates");
  assert.equal(samples, 2, "sample event fired per matching update");

  // max caps history length
  el.update({ count: { value: 1 } });
  el.update({ count: { value: 2 } });
  assert.deepEqual(el.history, [8, 1, 2], "history capped at max=3");
});
