//
// layout.mjs — the recursive split-pane layout tree (pure; no DOM). Shared by the
// renderer and the node test. See docs/design/production-hmi.md.
//
// A layout node is either:
//   a SPLIT: { split: "row"|"col", ratio: 0..1, a: <node>, b: <node> }
//            "row" = side-by-side (vertical divider; a=left,  b=right)
//            "col" = stacked      (horizontal divider; a=top, b=bottom)
//            ratio is child a's fraction; b gets 1 - ratio.
//   a LEAF:  { card: { type, bind?, config?, overlays? } }
//

export const isLeaf = (n) => !!(n && n.card);
export const isSplit = (n) => !!(n && (n.split === "row" || n.split === "col") && n.a && n.b);

// Visit every leaf (card) in document order. fn(card, path) — path is the
// sequence of "a"/"b" steps from the root, handy for stable ids in Compose mode.
export function eachLeaf(node, fn, path = []) {
  if (isLeaf(node)) { fn(node.card, path); return; }
  if (isSplit(node)) { eachLeaf(node.a, fn, [...path, "a"]); eachLeaf(node.b, fn, [...path, "b"]); }
}

export function countLeaves(node) {
  let n = 0; eachLeaf(node, () => n++); return n;
}

// Clamp a child-a ratio to a sane range so a pane can't collapse to nothing.
export const clampRatio = (r) => Math.min(0.9, Math.max(0.1, typeof r === "number" ? r : 0.5));

// ---- Compose-mode edits (pure; immutable — each returns a NEW tree) ---------
// Paths are arrays of "a"/"b" steps from the root (same as eachLeaf's path).

export const emptyCard = () => ({ type: "value", bind: {}, config: { title: "(empty)" } });

export function getNode(root, path) {
  let n = root;
  for (const s of path) { if (!isSplit(n)) return undefined; n = n[s]; }
  return n;
}

// Return a new tree with the node at `path` replaced by fn(oldNode).
function mapAt(root, path, fn) {
  if (path.length === 0) return fn(root);
  const [head, ...rest] = path;
  return { ...root, [head]: mapAt(root[head], rest, fn) };
}

// Split a leaf into {dir} with the old leaf as a and a fresh card as b.
export function splitLeaf(root, path, dir, newCard = emptyCard()) {
  return mapAt(root, path, (n) => ({ split: dir === "col" ? "col" : "row", ratio: 0.5, a: n, b: { card: newCard } }));
}

// Replace a leaf's card.
export function setCard(root, path, card) {
  return mapAt(root, path, () => ({ card }));
}

// Set a split's ratio (path points at the split).
export function setRatio(root, path, ratio) {
  return mapAt(root, path, (n) => (isSplit(n) ? { ...n, ratio: clampRatio(ratio) } : n));
}

// Remove a leaf: its parent split collapses to the sibling. Removing the only
// pane (root leaf) yields a fresh empty leaf.
export function removeLeaf(root, path) {
  if (path.length === 0) return { card: emptyCard() };
  const parent = path.slice(0, -1), sib = path[path.length - 1] === "a" ? "b" : "a";
  return mapAt(root, parent, (n) => (isSplit(n) ? n[sib] : n));
}

// Validate a tree; returns an array of human-readable problems ([] = ok).
export function validate(node, path = "root") {
  if (isLeaf(node)) {
    if (!node.card.type) return [`${path}: leaf has no card.type`];
    return [];
  }
  if (isSplit(node)) return [...validate(node.a, `${path}.a`), ...validate(node.b, `${path}.b`)];
  return [`${path}: node is neither a split {split,a,b} nor a leaf {card}`];
}
