// Node test for layout.mjs (recursive split-pane tree). Also validates the
// shipped dashboard.json. Run: node hmi/test/layout.test.mjs
import assert from "node:assert";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { isLeaf, isSplit, eachLeaf, countLeaves, clampRatio, validate,
         getNode, splitLeaf, setCard, setRatio, removeLeaf, emptyCard } from "../layout.mjs";

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

t("getNode walks a/b path", () => {
  assert.equal(getNode(tree, []).split, "col");
  assert.equal(getNode(tree, ["a", "a"]).card.type, "verdict");
  assert.equal(getNode(tree, ["b"]).card.type, "spc");
});

t("splitLeaf is immutable + inserts a split with old leaf as a", () => {
  const out = splitLeaf(tree, ["b"], "row");
  assert.ok(isSplit(getNode(out, ["b"])));
  assert.equal(getNode(out, ["b", "a"]).card.type, "spc");      // old leaf preserved
  assert.equal(getNode(out, ["b", "b"]).card.type, emptyCard().type); // fresh pane
  assert.ok(isLeaf(tree.b), "original tree unchanged");
});

t("setCard replaces a leaf's card", () => {
  const out = setCard(tree, ["b"], { type: "image", bind: { var: "result" } });
  assert.equal(getNode(out, ["b"]).card.type, "image");
  assert.equal(getNode(tree, ["b"]).card.type, "spc");          // immutable
});

t("setRatio clamps + targets the split", () => {
  assert.equal(getNode(setRatio(tree, [], 0.95), []).ratio, 0.9);
  assert.equal(getNode(setRatio(tree, ["a"], 0.42), ["a"]).ratio, 0.42);
});

t("removeLeaf collapses parent split to the sibling", () => {
  const out = removeLeaf(tree, ["a", "a"]);   // drop verdict -> a becomes the image leaf
  assert.equal(getNode(out, ["a"]).card.type, "image");
  assert.equal(countLeaves(out), 2);
  // removing the only pane yields a fresh empty leaf
  assert.ok(isLeaf(removeLeaf({ card: { type: "verdict" } }, [])));
});

console.log(`\n${pass} passed`);
