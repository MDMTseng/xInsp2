//
// layout.mjs — recursive N-ary split layout tree (pure; no DOM). Shared by the
// renderer and the node test. See docs/design/production-hmi.md.
//
// A layout node is either:
//   a SPLIT: { dir: "row"|"col", children: [node, …], weights?: [w, …] }
//            "row" = side-by-side, "col" = stacked. A split holds N panes; each
//            child's size is its weight / sum(weights). weights is optional
//            (defaults to equal); it need not be normalized.
//   a LEAF:  { card: { type, bind?, config?, overlays? } }
//
// Paths are arrays of child indices from the root (e.g. [0,2] = root.children[0]
// .children[2]); they match eachLeaf's path argument.

export const isLeaf  = (n) => !!(n && n.card);
export const isSplit = (n) => !!(n && (n.dir === "row" || n.dir === "col") && Array.isArray(n.children) && n.children.length >= 1);
// a TABS node: { tabs: [ { name, child: node }, … ], active?: index } — shows one
// child at a time behind a tab strip. Nests anywhere (a pane or the whole root).
export const isTabs  = (n) => !!(n && Array.isArray(n.tabs) && n.tabs.length >= 1 && n.tabs.every((t) => t && t.child));

export const emptyCard = () => ({ type: "value", bind: {}, config: { title: "(empty)" } });

// Raw (positive) weights aligned to children; missing/invalid → 1.
function rawWeights(node) {
  const n = node.children.length;
  const w = Array.isArray(node.weights) && node.weights.length === n ? node.weights.slice() : Array(n).fill(1);
  return w.map((x) => (typeof x === "number" && x > 0 ? x : 1));
}
// Normalized weights (fractions summing to 1) — what the renderer uses.
export function weightsOf(node) {
  const w = rawWeights(node);
  const s = w.reduce((a, b) => a + b, 0) || 1;
  return w.map((x) => x / s);
}

// A split's i-th child is children[i]; a tabs node's i-th child is tabs[i].child.
function childAt(node, i) { return isTabs(node) ? node.tabs[i].child : node.children[i]; }
function withChild(node, i, child) {
  if (isTabs(node)) { const tabs = node.tabs.slice(); tabs[i] = { ...tabs[i], child }; return { ...node, tabs }; }
  const children = node.children.slice(); children[i] = child; return { ...node, children };
}

export function eachLeaf(node, fn, path = []) {
  if (isLeaf(node)) { fn(node.card, path); return; }
  if (isSplit(node)) node.children.forEach((c, i) => eachLeaf(c, fn, [...path, i]));
  else if (isTabs(node)) node.tabs.forEach((t, i) => eachLeaf(t.child, fn, [...path, i]));
}
export function countLeaves(node) { let n = 0; eachLeaf(node, () => n++); return n; }

export function getNode(root, path) {
  let n = root;
  for (const i of path) { if (isSplit(n) || isTabs(n)) n = childAt(n, i); else return undefined; }
  return n;
}

// Return a new tree with the node at `path` replaced by fn(oldNode) (immutable).
function mapAt(root, path, fn) {
  if (path.length === 0) return fn(root);
  const [i, ...rest] = path;
  return withChild(root, i, mapAt(childAt(root, i), rest, fn));
}

// ---- Compose-mode edits (pure; immutable — each returns a NEW tree) ---------

// Split a leaf into a 2-pane split of `dir` (old leaf first, fresh card second).
export function splitLeaf(root, path, dir, newCard = emptyCard()) {
  return mapAt(root, path, (leaf) => ({ dir: dir === "col" ? "col" : "row", children: [leaf, { card: newCard }], weights: [1, 1] }));
}

// Add a pane next to the leaf at `path` in direction `dir`. If the leaf's parent
// is a split of the SAME dir, the new pane is inserted as a sibling (→ N panes);
// otherwise the leaf is wrapped in a fresh 2-pane split of `dir`.
export function addSibling(root, path, dir, newCard = emptyCard()) {
  dir = dir === "col" ? "col" : "row";
  if (path.length === 0) return { dir, children: [root, { card: newCard }], weights: [1, 1] };
  const parentPath = path.slice(0, -1), idx = path[path.length - 1];
  const parent = getNode(root, parentPath);
  if (isSplit(parent) && parent.dir === dir) {
    return mapAt(root, parentPath, (p) => {
      const children = p.children.slice(); children.splice(idx + 1, 0, { card: newCard });
      const w = rawWeights(p); w.splice(idx + 1, 0, w[idx]);   // new pane ≈ its neighbour's size
      return { ...p, children, weights: w };
    });
  }
  return mapAt(root, path, (leaf) => ({ dir, children: [leaf, { card: newCard }], weights: [1, 1] }));
}

// Remove the pane at `path`. Its parent split drops that child; a split left with
// one child collapses to it. Removing the only pane yields a fresh empty leaf.
export function removePane(root, path) {
  if (path.length === 0) return { card: emptyCard() };
  const parentPath = path.slice(0, -1), idx = path[path.length - 1];
  return mapAt(root, parentPath, (p) => {
    if (!isSplit(p)) return p;
    const children = p.children.filter((_, i) => i !== idx);
    const weights = rawWeights(p).filter((_, i) => i !== idx);
    return children.length === 1 ? children[0] : { ...p, children, weights };
  });
}

export function setCard(root, path, card) { return mapAt(root, path, () => ({ card })); }

// Replace a split's weights (path points at the split).
export function setWeights(root, path, weights) {
  return mapAt(root, path, (n) => (isSplit(n) ? { ...n, weights: weights.slice() } : n));
}

// ---- tabs / pages ----------------------------------------------------------

// Wrap the node at `path` in a tabs group (its content becomes the first page).
export function wrapInTabs(root, path) {
  return mapAt(root, path, (node) => ({ tabs: [{ name: "Page 1", child: node }, { name: "Page 2", child: { card: emptyCard() } }], active: 0 }));
}
export function addTab(root, path, name, child = { card: emptyCard() }) {
  return mapAt(root, path, (n) => isTabs(n) ? { ...n, tabs: [...n.tabs, { name: name || `Page ${n.tabs.length + 1}`, child }], active: n.tabs.length } : n);
}
export function removeTab(root, path, i) {
  return mapAt(root, path, (n) => {
    if (!isTabs(n)) return n;
    const tabs = n.tabs.filter((_, k) => k !== i);
    if (tabs.length === 1) return tabs[0].child;           // collapse to the sole tab's content
    return { ...n, tabs, active: Math.max(0, Math.min(n.active || 0, tabs.length - 1)) };
  });
}
export function renameTab(root, path, i, name) {
  return mapAt(root, path, (n) => isTabs(n) ? { ...n, tabs: n.tabs.map((t, k) => k === i ? { ...t, name } : t) } : n);
}
export function setActive(root, path, i) {
  return mapAt(root, path, (n) => isTabs(n) ? { ...n, active: i } : n);
}

// Validate a tree; returns an array of human-readable problems ([] = ok).
export function validate(node, path = "root") {
  if (isLeaf(node)) return node.card.type ? [] : [`${path}: leaf has no card.type`];
  if (isSplit(node)) return node.children.flatMap((c, i) => validate(c, `${path}.${i}`));
  if (isTabs(node)) return node.tabs.flatMap((t, i) => validate(t.child, `${path}.${t.name || i}`));
  return [`${path}: node is not a leaf {card}, split {dir,children}, or tabs {tabs}`];
}
