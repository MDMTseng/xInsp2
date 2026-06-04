// Node test for layout.mjs (recursive split-pane tree). Also validates the
// shipped dashboard.json. Run: node hmi/test/layout.test.mjs
import assert from "node:assert";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { isLeaf, isSplit, eachLeaf, countLeaves, clampRatio, validate } from "../layout.mjs";

const here = dirname(fileURLToPath(import.meta.url));
let pass = 0;
const t = (name, fn) => { fn(); console.log("ok -", name); pass++; };

const tree = {
  split: "col", ratio: 0.7,
  a: { split: "row", ratio: 0.3,
       a: { card: { type: "verdict", bind: { var: "verdict" } } },
       b: { card: { type: "image", bind: { var: "result" } } } },
  b: { card: { type: "spc", bind: { var: "fg_pct" } } },
};

t("isLeaf / isSplit classify nodes", () => {
  assert.ok(isSplit(tree));
  assert.ok(isLeaf(tree.b));
  assert.ok(!isSplit(tree.b));
  assert.ok(!isLeaf(tree));
});

t("eachLeaf visits all cards in order with a/b path", () => {
  const seen = [];
  eachLeaf(tree, (card, path) => seen.push([card.type, path.join("")]));
  assert.deepEqual(seen, [["verdict", "aa"], ["image", "ab"], ["spc", "b"]]);
});

t("countLeaves", () => assert.equal(countLeaves(tree), 3));

t("clampRatio keeps panes from collapsing", () => {
  assert.equal(clampRatio(0.5), 0.5);
  assert.equal(clampRatio(0), 0.1);
  assert.equal(clampRatio(2), 0.9);
  assert.equal(clampRatio(undefined), 0.5);
});

t("validate flags a bad node and a typeless leaf", () => {
  assert.deepEqual(validate(tree), []);
  assert.equal(validate({ split: "row", ratio: 0.5, a: { card: {} }, b: { foo: 1 } }).length, 2);
});

t("shipped dashboard.json is a valid layout tree", () => {
  const dash = JSON.parse(readFileSync(join(here, "..", "dashboard.json"), "utf8"));
  assert.ok(dash.layout, "has a layout tree");
  assert.deepEqual(validate(dash.layout), []);
  assert.ok(countLeaves(dash.layout) >= 5);
});

console.log(`\n${pass} passed`);
