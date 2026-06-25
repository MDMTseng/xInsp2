// editor.dom.mjs — teach editor (task #77) smoke: instantiates, renders the
// commit/cancel bar + canvas, setTool swaps the active tool, Cancel emits. (The
// draw/interaction is canvas/browser; the tool result logic is in tools.mjs.)
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom, tick } from "./_dom.mjs";

const w = setupDom();
await import("../dist/xi-components.esm.js");

test("xi-image-editor renders commit/cancel + canvas, Cancel emits", async () => {
  w.document.body.innerHTML = `<xi-image-editor tool="polygon" label="Template"></xi-image-editor>`;
  const el = w.document.body.firstElementChild;
  await tick(w);
  assert.ok(w.customElements.get("xi-image-editor"), "defined");

  const sr = el.shadowRoot;
  assert.ok(sr.querySelector("canvas"), "canvas rendered");
  assert.ok(sr.querySelector("button.commit"), "Commit button");
  assert.ok(sr.querySelector("button.cancel"), "Cancel button");

  assert.equal(typeof el.setFrame, "function", "tap-out setFrame");
  assert.equal(typeof el.setTool, "function", "tap-out setTool");
  el.setTool("rect"); // swap tool without error

  let cancelled = false;
  el.addEventListener("cancel", () => { cancelled = true; });
  sr.querySelector("button.cancel").click();
  assert.equal(cancelled, true, "Cancel emits the cancel event");
});
