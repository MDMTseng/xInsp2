// tools.mjs test — teach-tool state machines (task #77): pointer events (image
// coords) → result. Pure logic, deterministic.
import { test } from "node:test";
import assert from "node:assert/strict";
import { makeTool, registerTool, TOOLS } from "../src/lib/tools.mjs";

test("point tool: one click → {x,y}", () => {
  const t = makeTool("point");
  assert.equal(t.done(), false);
  t.onDown({ x: 12.4, y: 7.6 });
  assert.equal(t.done(), true);
  assert.deepEqual(t.result(), { x: 12, y: 8 }, "rounded image point");
});

test("rect tool: drag → normalized {x,y,w,h}", () => {
  const t = makeTool("rect");
  t.onDown({ x: 40, y: 30 });
  t.onMove({ x: 10, y: 50 });
  t.onUp({ x: 10, y: 50 });
  assert.equal(t.done(), true);
  assert.deepEqual(t.result(), { x: 10, y: 30, w: 30, h: 20 }, "normalized regardless of drag direction");
});

test("polygon tool: vertices + close → {points, closed}", () => {
  const t = makeTool("polygon");
  t.onDown({ x: 0, y: 0 }); t.onDown({ x: 10, y: 0 }); t.onDown({ x: 10, y: 10 });
  assert.equal(t.done(), false, "not done until closed");
  t.onDbl();
  assert.equal(t.done(), true);
  assert.deepEqual(t.result(), { points: [[0, 0], [10, 0], [10, 10]], closed: true });
});

test("polygon needs ≥3 points to close", () => {
  const t = makeTool("polygon");
  t.onDown({ x: 0, y: 0 }); t.onDown({ x: 5, y: 5 });
  t.onDbl();
  assert.equal(t.done(), false, "two points can't close");
});

test("registerTool adds a custom tool", () => {
  registerTool("dot", () => ({ type: "dot", onDown() {}, done: () => true, result: () => ({ ok: 1 }), draw() {} }));
  assert.ok(TOOLS.dot, "registered");
  assert.deepEqual(makeTool("dot").result(), { ok: 1 });
});
