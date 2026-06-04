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

// Validate a tree; returns an array of human-readable problems ([] = ok).
export function validate(node, path = "root") {
  if (isLeaf(node)) {
    if (!node.card.type) return [`${path}: leaf has no card.type`];
    return [];
  }
  if (isSplit(node)) return [...validate(node.a, `${path}.a`), ...validate(node.b, `${path}.b`)];
  return [`${path}: node is neither a split {split,a,b} nor a leaf {card}`];
}
