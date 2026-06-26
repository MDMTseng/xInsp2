//
// app.mjs — HMI host (v1, RUN mode): connect WS, decode the stream, lay out the
// dashboard grid, and feed each card. Compose/drag-drop + save_dashboard = v1.1.
//
// Cards, the layout engine, and the protocol decoders now live in the shared
// ui-components library (vendored bundle) — one source, used by the standalone
// HMI here and by external apps via `import { mountDashboard } from "@xinsp2/
// components"`. This page keeps its own compose-mode editor below.
import { parseVars, decodePreviewFrame, CARDS,
         isLeaf, isSplit, isTabs, weightsOf, validate,
         getNode, addSibling, setCard, setWeights, removePane,
         wrapInTabs, addTab, removeTab, renameTab, setActive } from "./lib/xi-components.esm.js";

const qs = new URLSearchParams(location.search);
// Default to a same-origin /ws (served by serve.mjs's proxy) so one HTTP tunnel
// exposes both the page and the WS. Override with ?ws=ws://host:port/ for a
// direct backend connection (e.g. serve.py's static-only mode).
const WS_URL = qs.get("ws") ||
  `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`;
const DASH = qs.get("dashboard") || "./dashboard.json";

const state = { run_id: -1, vars: {}, images: {}, run_ms: null, status: null, result: null, groups: [] };
let cards = [];
let raf = 0;

// ---- on-page diagnostics (so issues are copyable without DevTools) ----------
const diag = document.createElement("pre");
diag.id = "xi-diag";
diag.style.cssText = "position:fixed;left:0;right:0;bottom:0;max-height:38vh;overflow:auto;margin:0;" +
  "padding:6px 10px;background:#0b0b0bdd;color:#9fe;border-top:1px solid #333;font:11px/1.45 ui-monospace,monospace;" +
  "white-space:pre-wrap;word-break:break-all;z-index:99999;user-select:text";
const pad = (n) => String(n).padStart(2, "0");
function dlog(msg) {
  const d = new Date();
  const line = `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}  ${msg}`;
  diag.textContent += (diag.textContent ? "\n" : "") + line;
  diag.scrollTop = diag.scrollHeight;
  console.log("[hmi]", msg);
}
// The on-page panel is a bring-up aid — opt in with ?debug=1 so the operator
// view stays clean. dlog() still mirrors to the console regardless.
const DEBUG = qs.has("debug");
if (DEBUG) {
  addEventListener("DOMContentLoaded", () => document.body.appendChild(diag));
  if (document.body) document.body.appendChild(diag);
}
window.addEventListener("error", (e) => dlog(`JS ERROR: ${e.message} @ ${e.filename}:${e.lineno}`));
window.addEventListener("unhandledrejection", (e) => dlog(`PROMISE REJECT: ${e.reason}`));
dlog(`boot. href=${location.href}`);
dlog(`WS_URL=${WS_URL}  (host=${location.host} proto=${location.protocol})`);

const badge = () => document.getElementById("conn");
function setConn(txt, color) { const b = badge(); if (b) { b.textContent = txt; b.style.background = color; } }

function scheduleRender() {
  if (raf) return;
  raf = requestAnimationFrame(() => { raf = 0; for (const el of cards) { try { el.feed(state); } catch (e) { console.error(e); } } });
}

// Render a card leaf into an element (collected into `cards` for feed()).
function renderCard(card) {
  const C = CARDS[card.type];
  const el = document.createElement(C ? `xi-card-${card.type}` : "div");
  if (!C) { el.textContent = `unknown card: ${card.type}`; el.style.cssText = "background:#3a1e1e;color:#f88;padding:8px;border-radius:6px"; }
  el.binding = card.bind || {}; el.config = card.config || {};
  el.style.minWidth = "0"; el.style.minHeight = "0"; el.style.overflow = "hidden";
  if (C) cards.push(el);
  return el;
}

// ---- the live layout tree + mode (run | compose) ---------------------------
let layout = null;
let mode = qs.get("mode") === "compose" ? "compose" : "run";

// Compose overlay for a leaf: the live card behind a toolbar (type / var / title
// + split / remove). Edits mutate `layout` via the pure ops in layout.mjs.
function renderLeafCompose(card, path) {
  const wrap = document.createElement("div");
  wrap.style.cssText = "position:relative;min-width:0;min-height:0;overflow:hidden;border:1px dashed #5a5a5a;border-radius:6px";
  const cardEl = renderCard(card);
  cardEl.style.cssText += ";position:absolute;inset:0;pointer-events:none;opacity:.8";
  wrap.appendChild(cardEl);

  const bar = document.createElement("div");
  bar.style.cssText = "position:absolute;top:0;left:0;right:0;z-index:2;display:flex;flex-wrap:wrap;gap:4px;" +
    "align-items:center;padding:4px;background:#000a;font:11px system-ui,sans-serif";
  const sel = document.createElement("select");
  for (const k of Object.keys(CARDS)) { const o = document.createElement("option"); o.value = o.textContent = k; if (k === card.type) o.selected = true; sel.appendChild(o); }
  sel.onchange = () => editCard(path, { type: sel.value });
  const inp = (ph, val, w) => { const i = document.createElement("input"); i.placeholder = ph; i.value = val || ""; i.size = w; i.style.cssText = "background:#222;color:#ddd;border:1px solid #444;border-radius:3px;padding:1px 4px;font:11px system-ui"; return i; };
  const varIn = inp("var", card.bind && card.bind.var, 6);
  varIn.onchange = () => editCard(path, { bind: { ...card.bind, var: varIn.value || undefined } });
  const titleIn = inp("title", card.config && card.config.title, 7);
  titleIn.onchange = () => editCard(path, { config: { ...card.config, title: titleIn.value } });
  const btn = (t, fn, tip) => { const b = document.createElement("button"); b.textContent = t; b.title = tip || ""; b.style.cssText = "padding:1px 7px;cursor:pointer"; b.onclick = fn; return b; };
  bar.append(sel, varIn, titleIn,
    btn("+⬌", () => { layout = addSibling(layout, path, "row"); reRender(); }, "add pane to the right"),
    btn("+⬍", () => { layout = addSibling(layout, path, "col"); reRender(); }, "add pane below"),
    btn("⊞", () => { layout = wrapInTabs(layout, path); reRender(); }, "wrap this pane in tabs"),
    btn("✕", () => { layout = removePane(layout, path); reRender(); }, "remove pane"));
  wrap.appendChild(bar);
  return wrap;
}
function editCard(path, patch) {
  const cur = getNode(layout, path).card;
  layout = setCard(layout, path, { ...cur, ...patch });
  reRender();
}

// A draggable divider between panes i and i+1 of an N-ary split. Dragging shifts
// the boundary between just those two panes (their combined size is preserved).
function makeDivider(path, i, col, box, els, fr) {
  const div = document.createElement("div");
  div.style.cssText = `flex:0 0 ${mode === "compose" ? 9 : 8}px;background:${mode === "compose" ? "#3a6ea5" : "#222"};` +
    (mode === "compose" ? `touch-action:none;cursor:${col ? "row-resize" : "col-resize"}` : "pointer-events:none");
  if (mode !== "compose") return div;
  // Pointer events cover mouse + touch + pen (so divider-drag works on a phone).
  div.onpointerdown = (e) => {
    e.preventDefault();
    try { div.setPointerCapture(e.pointerId); } catch {}
    const rect = box.getBoundingClientRect();
    let before = 0; for (let k = 0; k < i; k++) before += fr[k];
    const pair = fr[i] + fr[i + 1];
    const cur = fr.slice();
    const move = (ev) => {
      const f = col ? (ev.clientY - rect.top) / rect.height : (ev.clientX - rect.left) / rect.width;
      const a = Math.min(before + pair - 0.05, Math.max(before + 0.05, f)) - before; // new fr[i]
      cur[i] = a; cur[i + 1] = pair - a;
      els[i].style.flex = `${cur[i]} 1 0`; els[i + 1].style.flex = `${cur[i + 1]} 1 0`;
      div._w = cur.slice();
    };
    const up = () => { document.removeEventListener("pointermove", move); document.removeEventListener("pointerup", up); if (div._w) { layout = setWeights(layout, path, div._w); reRender(); } };
    document.addEventListener("pointermove", move); document.addEventListener("pointerup", up);
  };
  return div;
}

// A tabs/pages node: a tab strip + ALL tab bodies mounted at once. Switching a
// tab only toggles visibility (display:none) — it never destroys the inactive
// pages, so their cards keep their accumulated state + live data while hidden.
function renderTabs(node, path) {
  let active = Math.min(node.active || 0, node.tabs.length - 1);
  const wrap = document.createElement("div");
  wrap.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
  const strip = document.createElement("div");
  strip.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
  const bodyHost = document.createElement("div");
  bodyHost.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";

  const tabEls = [], bodies = [];
  const restyle = () => {
    tabEls.forEach((t, k) => { const on = k === active;
      t.style.background = on ? "#1e1e1e" : "#121212"; t.style.color = on ? "#ddd" : "#888"; });
    bodies.forEach((b, k) => { b.style.display = k === active ? "" : "none"; });
  };
  const show = (i) => { if (i === active) return; active = i; layout = setActive(layout, path, i); restyle(); };

  node.tabs.forEach((tb, i) => {
    const tab = document.createElement("div");
    tab.style.cssText = "display:flex;align-items:center;gap:5px;padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;" +
      "border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif";
    const label = document.createElement("span"); label.textContent = tb.name || `Page ${i + 1}`;
    tab.appendChild(label);
    tab.onclick = () => show(i);
    if (mode === "compose") {
      label.title = "double-click to rename";
      label.ondblclick = (e) => { e.stopPropagation(); const nn = prompt("Tab name", tb.name || ""); if (nn != null) { layout = renameTab(layout, path, i, nn); reRender(); } };
      if (node.tabs.length > 1) {
        const x = document.createElement("span"); x.textContent = "✕"; x.title = "remove tab"; x.style.cssText = "opacity:.55;font-size:10px";
        x.onclick = (e) => { e.stopPropagation(); layout = removeTab(layout, path, i); reRender(); };
        tab.appendChild(x);
      }
    }
    tabEls.push(tab); strip.appendChild(tab);

    const body = renderNode(tb.child, [...path, i]);  // all bodies rendered + fed
    body.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden";
    bodies.push(body); bodyHost.appendChild(body);
  });

  if (mode === "compose") {
    const add = document.createElement("button"); add.textContent = "+"; add.title = "add tab";
    add.style.cssText = "margin-left:4px;padding:2px 9px;cursor:pointer";
    add.onclick = () => { layout = addTab(layout, path); reRender(); };
    strip.appendChild(add);
  }
  restyle();
  wrap.append(strip, bodyHost);
  return wrap;
}

// Recursively render a layout node. A split lays out its N children along `dir`,
// sized by weights, with a divider between each adjacent pair. In compose mode
// leaves get a toolbar and dividers are draggable; in run mode dividers are fixed.
function renderNode(node, path = []) {
  if (isLeaf(node)) return mode === "compose" ? renderLeafCompose(node.card, path) : renderCard(node.card);
  if (isTabs(node)) return renderTabs(node, path);
  if (!isSplit(node)) { const e = document.createElement("div"); e.textContent = "bad layout node"; e.style.color = "#f88"; return e; }
  const col = node.dir === "col";
  const box = document.createElement("div");
  box.style.cssText = `display:flex;flex-direction:${col ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
  const fr = weightsOf(node);
  const els = node.children.map((c, i) => {
    const el = renderNode(c, [...path, i]);
    el.style.flex = `${fr[i]} 1 0`; el.style.minWidth = "0"; el.style.minHeight = "0";
    return el;
  });
  els.forEach((el, i) => {
    box.appendChild(el);
    if (i < els.length - 1) box.appendChild(makeDivider(path, i, col, box, els, fr));
  });
  return box;
}

// Preview subscription. The backend streams nothing unsubscribed, so subscribe
// to the var of every image card in the current layout (re-diffed on layout
// change, re-asserted on reconnect).
let activeWs = null, subId = 900000, imgSubs = [];
function collectImageVars(node, out = []) {
  if (!node) return out;
  if (isLeaf(node)) { const c = node.card; if (c && c.type === "image" && c.bind && c.bind.var) out.push(c.bind.var); }
  else if (isTabs(node)) (node.tabs || []).forEach((t) => collectImageVars(t.child || t, out));
  else if (isSplit(node)) (node.children || []).forEach((c) => collectImageVars(c, out));
  return out;
}
function pushImageSubs() {
  if (activeWs && activeWs.readyState === 1)
    activeWs.send(JSON.stringify({ type: "cmd", id: ++subId, name: "subscribe", args: { names: imgSubs } }));
}
function updateImageSubs() { imgSubs = collectImageVars(layout, []); pushImageSubs(); }

function reRender() {
  const root = document.getElementById("grid");
  root.style.cssText += ";display:flex;min-width:0;min-height:0";
  cards = [];
  root.replaceChildren();
  updateImageSubs();
  if (!layout) return;
  const problems = validate(layout);
  document.getElementById("err").textContent = problems.length ? problems.join("; ") : "";
  const el = renderNode(layout, []); el.style.flex = "1 1 0"; el.style.minWidth = "0"; el.style.minHeight = "0";
  root.appendChild(el);
  if (modeBtn) modeBtn.textContent = mode === "compose" ? "▶ Run" : "✎ Compose";
  if (exportWrap) exportWrap.style.display = mode === "compose" ? "flex" : "none";
  scheduleRender();
}

// Apply a dashboard object ({title, layout}) — used by both the static fetch
// (fallback) and the BE's get_dashboard reply (the project's own dashboard).
function applyDash(dash) {
  if (!dash || !dash.layout) return;
  document.title = (dash.title || "xInsp2 HMI");
  layout = dash.layout;
  reRender();
}
async function buildLayout() {
  try { applyDash(await (await fetch(DASH, { cache: "no-store" })).json()); }
  catch (e) { document.getElementById("err").textContent = `Could not load ${DASH}: ${e}`; }
}
// Optional ?board=<name> selects the project's dashboard.<name>.json (BE-served).
const BOARD = qs.get("board") || "";

// ---- header controls: mode toggle + export (compose) -----------------------
let modeBtn = null, exportWrap = null;
function buildControls() {
  const hdr = document.querySelector("header"); if (!hdr) return;
  const conn = document.getElementById("conn");
  const mkBtn = (t) => { const b = document.createElement("button"); b.textContent = t; b.style.cssText = "padding:3px 10px;cursor:pointer;font:12px system-ui"; return b; };
  modeBtn = mkBtn("✎ Compose");
  modeBtn.onclick = () => { mode = mode === "run" ? "compose" : "run"; reRender(); };
  exportWrap = document.createElement("span");
  exportWrap.style.cssText = "display:none;gap:6px";
  const dump = () => JSON.stringify({ title: document.title, layout }, null, 2);
  const copyBtn = mkBtn("Copy JSON");
  copyBtn.onclick = async () => { try { await navigator.clipboard.writeText(dump()); copyBtn.textContent = "Copied ✓"; setTimeout(() => copyBtn.textContent = "Copy JSON", 1200); } catch { dlog("clipboard blocked — use Download"); } };
  const dlBtn = mkBtn("Download");
  dlBtn.onclick = () => { const a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([dump()], { type: "application/json" })); a.download = "dashboard.json"; a.click(); URL.revokeObjectURL(a.href); };
  exportWrap.append(copyBtn, dlBtn);
  const group = document.createElement("span"); group.style.cssText = "display:flex;gap:6px;align-items:center;margin-left:auto";
  group.append(modeBtn, exportWrap);
  hdr.insertBefore(group, conn);
}
buildControls();

function connect() {
  setConn("connecting…", "#7a6a1e");
  dlog(`WS connect -> ${WS_URL}`);
  let ws;
  try { ws = new WebSocket(WS_URL); }
  catch (e) { dlog(`WS constructor threw: ${e}`); setConn("● bad url", "#6a1e1e"); return; }
  ws.binaryType = "arraybuffer";
  activeWs = ws;
  // Poll dispatch_stats so the groups card can show live per-group concurrency.
  // Cheap, so just poll whenever connected (the rsp is ignored if no groups card).
  let statsTimer = 0, cmdId = 1, dashCmdId = -1;
  const startStatsPoll = () => {
    if (statsTimer) return;
    statsTimer = setInterval(() => {
      if (ws.readyState === 1) ws.send(JSON.stringify({ type: "cmd", id: ++cmdId, name: "dispatch_stats", args: {} }));
    }, 150);
  };
  // Ask the BE for the PROJECT's dashboard so the HMI only needs the WS URL (no
  // filesystem coupling). If the project has one it overrides the static fetch.
  const requestDashboard = () => {
    dashCmdId = ++cmdId;
    ws.send(JSON.stringify({ type: "cmd", id: dashCmdId, name: "get_dashboard", args: BOARD ? { name: BOARD } : {} }));
  };
  ws.onopen = () => { dlog("WS OPEN ✓"); setConn("● live", "#1e6a3a"); startStatsPoll(); requestDashboard(); pushImageSubs(); };
  ws.onclose = (e) => { dlog(`WS CLOSE code=${e.code} reason=${e.reason || "-"} clean=${e.wasClean}`); if (statsTimer) { clearInterval(statsTimer); statsTimer = 0; } setConn("● disconnected", "#6a1e1e"); setTimeout(connect, 1500); };
  ws.onerror = () => { dlog("WS ERROR event"); ws.close(); };
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.type === "vars") { const { run_id, items } = parseVars(m); state.run_id = run_id; state.vars = items;
        // Map each image var's gid → canonical "src" gid so a single deduped
        // preview frame (one buffer VAR'd by many plugins) fans out to all of them.
        const g2c = {};
        for (const nm of Object.keys(items || {})) { const it = items[nm]; if (it && it.gid != null) g2c[it.gid] = it.src != null ? it.src : it.gid; }
        state.gidToCanon = g2c; scheduleRender(); }
      else if (m.type === "event" && m.name === "run_finished" && m.data) { if (typeof m.data.ms === "number") state.run_ms = m.data.ms; }
      // The one per-run verdict (see docs/roadmap/run-result.md). Carries its own
      // run_id (absent on a dropped frame) so cards can count distinct runs.
      else if (m.type === "event" && m.name === "run_result" && m.data) { state.result = m.data; scheduleRender(); }
      else if (m.type === "event" && (m.name === "safe_state" || m.name === "status")) { state.status = m.data; scheduleRender(); }
      // dispatch_stats reply → feed the groups card.
      else if (m.type === "rsp" && m.data && Array.isArray(m.data.groups)) { state.groups = m.data.groups; scheduleRender(); }
      // get_dashboard reply → the project's own dashboard wins over the static one.
      else if (m.type === "rsp" && m.id === dashCmdId) {
        if (m.data?.found && m.data.dashboard) { dlog("dashboard from BE (project)"); applyDash(m.data.dashboard); }
        else dlog("BE has no project dashboard — keeping static");
      }
    } else {
      try { const f = decodePreviewFrame(ev.data);
        const canon = state.gidToCanon && f.gid in state.gidToCanon ? state.gidToCanon[f.gid] : f.gid;
        // Decode once here and store the GC-managed <img>; the cards' setFrame
        // draws the shared handle instead of each decoding the dataUrl again.
        const im = new Image();
        im.onload  = () => { state.images[canon] = im; scheduleRender(); };
        im.onerror = () => { state.images[canon] = f.dataUrl; scheduleRender(); };
        im.src = f.dataUrl;
      } catch (e) { console.error(e); }
    }
  };
}

// Run layout + connect independently so a layout error can't block the WS, and
// every step is visible on-page.
dlog("starting buildLayout()…");
buildLayout()
  .then(() => dlog(`buildLayout() done (${cards.length} cards)`))
  .catch((e) => dlog(`buildLayout() FAILED: ${e && e.stack ? e.stack : e}`));
dlog("starting connect()…");
connect();
