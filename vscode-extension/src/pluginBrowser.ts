// pluginBrowser.ts — HTML for the "Plugin Browser" webview.
//
// No `vscode` import on purpose (renderable standalone). The panel shows:
//   1. ADDED plugins — the project.json `plugins` declarations, each with its
//      path + a compile toggle + remove.
//   2. BROWSE — a collapsible folder tree per `plugin_dirs` search root; folders
//      that contain a plugin.json are addable. A "+ Add folder" adder sits at the
//      bottom (appends a root to plugin_dirs).
// The extension fills the model and handles the postMessage actions.

export interface PBPlugin { label: string; path: string; compile: boolean; }
export interface PBTreeNode {
    name: string;          // folder name
    rel: string;           // path relative to its root (what a `plugins` entry's path becomes)
    isPlugin: boolean;     // has a plugin.json
    needsCompile: boolean; // has .cpp / CMakeLists (source/cmake → default compile:true)
    children: PBTreeNode[];
}
export interface PBRoot {
    raw: string;           // as written in plugin_dirs (or "(default)" for the fallback)
    resolved: string;      // absolute, expanded
    exists: boolean;
    tree: PBTreeNode[];
}
export interface PBModel {
    projectName: string;
    added: PBPlugin[];
    addedPaths: string[];  // paths already declared (to mark tree nodes as added)
    roots: PBRoot[];
}

export function renderPluginBrowserHtml(model: PBModel): string {
    const json = JSON.stringify(model).replace(/</g, '\\u003c');
    return `<!DOCTYPE html><html><head><meta charset="utf-8"><style>
  body { font-family: var(--vscode-font-family); font-size: 13px; color: var(--vscode-foreground);
         padding: 8px 12px; }
  h2 { font-size: 12px; text-transform: uppercase; letter-spacing: .5px; opacity: .8;
       border-bottom: 1px solid var(--vscode-panel-border); padding-bottom: 4px; margin: 16px 0 8px; }
  .row { display: flex; align-items: center; gap: 8px; padding: 3px 0; }
  .added { display: flex; align-items: center; gap: 10px; padding: 4px 6px; border-radius: 4px; }
  .added:hover { background: var(--vscode-list-hoverBackground); }
  .added .name { font-weight: 600; min-width: 120px; }
  .added .path { opacity: .7; font-family: var(--vscode-editor-font-family); flex: 1; }
  .muted { opacity: .6; }
  button { background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground);
           border: none; padding: 2px 8px; border-radius: 3px; cursor: pointer; font-size: 12px; }
  button.primary { background: var(--vscode-button-background); color: var(--vscode-button-foreground); }
  button:hover { filter: brightness(1.1); }
  .tree { margin-left: 4px; }
  .node { padding: 1px 0; }
  .node .twist { display: inline-block; width: 14px; cursor: pointer; user-select: none; opacity: .7; }
  .node .label { cursor: default; }
  .node.dir > .label { cursor: pointer; }
  .node.plugin > .label { color: var(--vscode-charts-blue); }
  .kids { margin-left: 16px; }
  .root { margin: 6px 0; }
  .root .rhead { font-weight: 600; }
  .root .rpath { opacity: .55; font-size: 11px; font-family: var(--vscode-editor-font-family); }
  .badge { font-size: 10px; opacity: .6; }
  .addfolder { margin-top: 12px; }
  input[type=checkbox] { vertical-align: middle; }
</style></head><body>
<div id="app"></div>
<script>
const vscode = acquireVsCodeApi();
let M = ${json};
const collapsed = new Set();   // keys of collapsed dir nodes

function el(tag, props, ...kids) {
  const e = document.createElement(tag);
  if (props) for (const k in props) {
    if (k === 'class') e.className = props[k];
    else if (k === 'onclick') e.onclick = props[k];
    else if (k === 'onchange') e.onchange = props[k];
    else e.setAttribute(k, props[k]);
  }
  for (const c of kids) if (c != null) e.append(c.nodeType ? c : document.createTextNode(c));
  return e;
}

function renderAdded() {
  const wrap = el('div');
  wrap.append(el('h2', null, 'Added plugins (' + M.added.length + ')'));
  if (!M.added.length) wrap.append(el('div', { class: 'muted' }, 'None yet — add one from a root below.'));
  for (const p of M.added) {
    wrap.append(el('div', { class: 'added' },
      el('span', { class: 'name' }, p.label),
      el('span', { class: 'path' }, p.path),
      el('label', null,
        el('input', { type: 'checkbox', ...(p.compile ? { checked: 'checked' } : {}),
          onchange: () => vscode.postMessage({ type: 'toggleCompile', label: p.label }) }),
        ' compile'),
      el('button', { onclick: () => vscode.postMessage({ type: 'remove', label: p.label }) }, 'Remove')
    ));
  }
  return wrap;
}

function renderNode(node) {
  const added = M.addedPaths.includes(node.rel);
  if (node.isPlugin) {
    const row = el('div', { class: 'node plugin' });
    row.append(el('span', { class: 'twist' }, ' '));
    row.append(el('span', { class: 'label' }, '🧩 ' + node.name + ' '));
    if (added) row.append(el('span', { class: 'badge' }, '✓ added'));
    else row.append(el('button', { class: 'primary',
      onclick: () => vscode.postMessage({ type: 'add', path: node.rel, compile: node.needsCompile }) }, 'Add'));
    return row;
  }
  const key = node.rel || node.name;
  const isCol = collapsed.has(key);
  const wrap = el('div', { class: 'node dir' });
  const head = el('div');
  head.append(el('span', { class: 'twist', onclick: () => { if (isCol) collapsed.delete(key); else collapsed.add(key); draw(); } }, isCol ? '▸' : '▾'));
  head.append(el('span', { class: 'label', onclick: () => { if (isCol) collapsed.delete(key); else collapsed.add(key); draw(); } }, '📁 ' + node.name));
  wrap.append(head);
  if (!isCol && node.children && node.children.length) {
    const kids = el('div', { class: 'kids' });
    for (const c of node.children) kids.append(renderNode(c));
    wrap.append(kids);
  }
  return wrap;
}

function renderRoots() {
  const wrap = el('div');
  wrap.append(el('h2', null, 'Browse (' + M.roots.length + ' root' + (M.roots.length === 1 ? '' : 's') + ')'));
  for (const r of M.roots) {
    const rb = el('div', { class: 'root' });
    rb.append(el('div', { class: 'rhead' }, r.raw, r.exists ? '' : el('span', { class: 'badge' }, '  (missing)')));
    rb.append(el('div', { class: 'rpath' }, r.resolved));
    if (r.exists) {
      const tree = el('div', { class: 'tree' });
      if (!r.tree.length) tree.append(el('div', { class: 'muted' }, 'no plugins here'));
      for (const n of r.tree) tree.append(renderNode(n));
      rb.append(tree);
    }
    wrap.append(rb);
  }
  wrap.append(el('div', { class: 'addfolder' },
    el('button', { onclick: () => vscode.postMessage({ type: 'addFolder' }) }, '+ Add folder…')));
  return wrap;
}

function draw() {
  const app = document.getElementById('app');
  app.innerHTML = '';
  app.append(el('div', { class: 'muted' }, 'Project: ' + M.projectName));
  app.append(renderAdded());
  app.append(renderRoots());
}

window.addEventListener('message', (e) => {
  if (e.data && e.data.type === 'model') { M = e.data.model; draw(); }
});
draw();
</script></body></html>`;
}
