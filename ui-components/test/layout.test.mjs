// Node test for layout.mjs (N-ary split tree). Also validates dashboard.json.
// Run: node hmi/test/layout.test.mjs
import assert from "node:assert";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { isLeaf, isSplit, isTabs, eachLeaf, countLeaves, weightsOf, validate,
         getNode, splitLeaf, addSibling, removePane, setCard, setWeights, emptyCard,
         wrapInTabs, addTab, removeTab, renameTab, setActive } from "../src/dashboard/layout.mjs";

const here = dirname(fileURLToPath(import.meta.url));
let pass = 0;
const t = (name, fn) => { fn(); console.log("ok -", name); pass++; };
const leaf = (type) => ({ card: { type } });

// row of 3: [verdict, image, spc]
const tree = { dir: "row", weights: [1, 3, 1], children: [leaf("verdict"), leaf("image"), leaf("spc")] };

t("isLeaf / isSplit classify N-ary nodes", () => {
  assert.ok(isSplit(tree));
  assert.equal(tree.children.length, 3);
  assert.ok(isLeaf(tree.children[0]));
  assert.ok(!isSplit(leaf("x")));
});

t("weightsOf normalizes; defaults to equal when absent", () => {
  assert.deepEqual(weightsOf(tree), [0.2, 0.6, 0.2]);
  assert.deepEqual(weightsOf({ dir: "row", children: [leaf("a"), leaf("b")] }), [0.5, 0.5]);
});

t("eachLeaf visits all panes with index path", () => {
  const seen = []; eachLeaf(tree, (c, p) => seen.push([c.type, p.join("")]));
  assert.deepEqual(seen, [["verdict", "0"], ["image", "1"], ["spc", "2"]]);
  assert.equal(countLeaves(tree), 3);
});

t("getNode walks index path", () => {
  assert.equal(getNode(tree, [1]).card.type, "image");
  assert.equal(getNode(tree, []).dir, "row");
});

t("addSibling appends to a same-dir parent -> N panes", () => {
  const out = addSibling(tree, [1], "row");           // add after image
  assert.equal(countLeaves(out), 4);
  assert.equal(getNode(out, [2]).card.type, emptyCard().type);   // inserted right after image
  assert.equal(getNode(out, [3]).card.type, "spc");
  assert.equal(out.weights.length, 4);
  assert.equal(countLeaves(tree), 3);                 // immutable
});

t("addSibling with a different dir wraps the leaf in a nested split", () => {
  const out = addSibling(tree, [0], "col");           // verdict becomes a col-split
  assert.ok(isSplit(getNode(out, [0])));
  assert.equal(getNode(out, [0]).dir, "col");
  assert.equal(getNode(out, [0, 0]).card.type, "verdict");
});

t("splitLeaf makes a 2-pane split", () => {
  const out = splitLeaf(leaf("verdict"), [], "col");
  assert.ok(isSplit(out) && out.dir === "col" && out.children.length === 2);
});

t("removePane drops a child; collapses a split left with one", () => {
  const out = removePane(tree, [1]);                  // drop image -> 2 panes
  assert.equal(countLeaves(out), 2);
  assert.deepEqual(out.children.map((c) => c.card.type), ["verdict", "spc"]);
  const two = { dir: "row", children: [leaf("a"), leaf("b")] };
  assert.ok(isLeaf(removePane(two, [0])));            // collapses to the sole sibling
  assert.ok(isLeaf(removePane(leaf("x"), [])));       // removing the only pane -> empty leaf
});

t("setCard / setWeights are immutable + targeted", () => {
  assert.equal(getNode(setCard(tree, [2], { type: "yield" }), [2]).card.type, "yield");
  assert.deepEqual(getNode(setWeights(tree, [], [2, 2, 1]), []).weights, [2, 2, 1]);
  assert.deepEqual(tree.weights, [1, 3, 1]);
});

t("validate flags bad nodes + typeless leaves", () => {
  assert.deepEqual(validate(tree), []);
  assert.equal(validate({ dir: "row", children: [{ card: {} }, { nope: 1 }] }).length, 2);
});

t("tabs: wrap/add/rename/setActive + traversal + getNode", () => {
  const w = wrapInTabs(tree, []);                       // whole tree -> Page 1, + Page 2
  assert.ok(isTabs(w));
  assert.equal(w.tabs.length, 2);
  assert.equal(w.tabs[0].name, "Page 1");
  assert.equal(getNode(w, [0]).dir, "row");             // path into a tab = tab index -> its child
  assert.equal(getNode(w, [0, 1]).card.type, "image");  // nested: tab 0 -> pane 1
  assert.equal(countLeaves(w), countLeaves(tree) + 1);  // + the empty Page 2
  const a = addTab(renameTab(w, [], 0, "Main"), []);     // rename Page1->Main, add a tab
  assert.equal(a.tabs[0].name, "Main");
  assert.equal(a.tabs.length, 3);
  assert.equal(a.active, 2);                             // addTab focuses the new tab
  assert.equal(getNode(setActive(a, [], 1), []).active, 1);
});

t("tabs: removeTab drops a tab; collapses to content when one remains", () => {
  const w = wrapInTabs(leaf("verdict"), []);            // 2 tabs
  assert.ok(isLeaf(removeTab(w, [], 1)));               // back to the bare leaf
  const three = addTab(w, []);                          // 3 tabs
  assert.equal(removeTab(three, [], 0).tabs.length, 2);
});

t("validate accepts a tabs tree; flags a tab with a bad child", () => {
  assert.deepEqual(validate(wrapInTabs(tree, [])), []);
  assert.equal(validate({ tabs: [{ name: "x", child: { nope: 1 } }] }).length, 1);
});

t("shipped dashboard.json is a valid N-ary tree", () => {
  const dash = JSON.parse(readFileSync(join(here, "..", "..", "hmi", "dashboard.json"), "utf8"));
  assert.ok(dash.layout, "has a layout tree");
  assert.deepEqual(validate(dash.layout), []);
  assert.ok(countLeaves(dash.layout) >= 5);
});

console.log(`\n${pass} passed`);
