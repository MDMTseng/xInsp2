//
// app.mjs — HMI host (v1, RUN mode): connect WS, decode the stream, lay out the
// dashboard grid, and feed each card. Compose/drag-drop + save_dashboard = v1.1.
//
import { parseVars, decodePreviewFrame } from "./protocol.mjs";
import { CARDS } from "./cards.mjs";
import { isLeaf, isSplit, clampRatio, validate,
         getNode, splitLeaf, setCard, setRatio, removeLeaf } from "./layout.mjs";

const qs = new URLSearchParams(location.search);
// Default to a same-origin /ws (served by serve.mjs's proxy) so one HTTP tunnel
// exposes both the page and the WS. Override with ?ws=ws://host:port/ for a
// direct backend connection (e.g. serve.py's static-only mode).
const WS_URL = qs.get("ws") ||
  `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`;
const DASH = qs.get("dashboard") || "./dashboard.json";

const state = { run_id: -1, vars: {}, images: {}, run_ms: null, status: null };
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
    btn("⬌", () => { layout = splitLeaf(layout, path, "row"); reRender(); }, "split left/right"),
    btn("⬍", () => { layout = splitLeaf(layout, path, "col"); reRender(); }, "split top/bottom"),
    btn("✕", () => { layout = removeLeaf(layout, path); reRender(); }, "remove pane"));
  wrap.appendChild(bar);
  return wrap;
}
function editCard(path, patch) {
  const cur = getNode(layout, path).card;
  layout = setCard(layout, path, { ...cur, ...patch });
  reRender();
}

// Recursively render a layout node. In compose mode leaves get a toolbar and
// dividers are draggable; in run mode dividers are fixed.
function renderNode(node, path = []) {
  if (isLeaf(node)) return mode === "compose" ? renderLeafCompose(node.card, path) : renderCard(node.card);
  if (!isSplit(node)) { const e = document.createElement("div"); e.textContent = "bad layout node"; e.style.color = "#f88"; return e; }
  const col = node.split === "col";
  const box = document.createElement("div");
  box.style.cssText = `display:flex;flex-direction:${col ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
  const r = clampRatio(node.ratio);
  const a = renderNode(node.a, [...path, "a"]); a.style.flex = `${r} 1 0`;
  const b = renderNode(node.b, [...path, "b"]); b.style.flex = `${1 - r} 1 0`;
  const div = document.createElement("div");
  div.style.cssText = `flex:0 0 ${mode === "compose" ? 7 : 8}px;background:${mode === "compose" ? "#3a6ea5" : "#222"};` +
    (mode === "compose" ? `cursor:${col ? "row-resize" : "col-resize"}` : "pointer-events:none");
  if (mode === "compose") {
    div.onmousedown = (e) => {
      e.preventDefault();
      const rect = box.getBoundingClientRect();
      const move = (ev) => {
        const f = col ? (ev.clientY - rect.top) / rect.height : (ev.clientX - rect.left) / rect.width;
        const rr = Math.min(0.9, Math.max(0.1, f)); a.style.flex = `${rr} 1 0`; b.style.flex = `${1 - rr} 1 0`; div._r = rr;
      };
      const up = () => { document.removeEventListener("mousemove", move); document.removeEventListener("mouseup", up); if (div._r != null) { layout = setRatio(layout, path, div._r); reRender(); } };
      document.addEventListener("mousemove", move); document.addEventListener("mouseup", up);
    };
  }
  box.append(a, div, b);
  return box;
}

function reRender() {
  const root = document.getElementById("grid");
  root.style.cssText += ";display:flex;min-width:0;min-height:0";
  cards = [];
  root.replaceChildren();
  if (!layout) return;
  const problems = validate(layout);
  document.getElementById("err").textContent = problems.length ? problems.join("; ") : "";
  const el = renderNode(layout, []); el.style.flex = "1 1 0"; el.style.minWidth = "0"; el.style.minHeight = "0";
  root.appendChild(el);
  if (modeBtn) modeBtn.textContent = mode === "compose" ? "▶ Run" : "✎ Compose";
  if (exportWrap) exportWrap.style.display = mode === "compose" ? "flex" : "none";
  scheduleRender();
}

async function buildLayout() {
  let dash;
  try { dash = await (await fetch(DASH, { cache: "no-store" })).json(); }
  catch (e) { document.getElementById("err").textContent = `Could not load ${DASH}: ${e}`; return; }
  document.title = (dash.title || "xInsp2 HMI");
  layout = dash.layout || null;
  reRender();
}

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
  ws.onopen = () => { dlog("WS OPEN ✓"); setConn("● live", "#1e6a3a"); };
  ws.onclose = (e) => { dlog(`WS CLOSE code=${e.code} reason=${e.reason || "-"} clean=${e.wasClean}`); setConn("● disconnected", "#6a1e1e"); setTimeout(connect, 1500); };
  ws.onerror = () => { dlog("WS ERROR event"); ws.close(); };
  ws.onmessage = (ev) => {
    if (typeof ev.data === "string") {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.type === "vars") { const { run_id, items } = parseVars(m); state.run_id = run_id; state.vars = items; scheduleRender(); }
      else if (m.type === "event" && m.name === "run_finished" && m.data) { if (typeof m.data.ms === "number") state.run_ms = m.data.ms; }
      else if (m.type === "event" && (m.name === "safe_state" || m.name === "status")) { state.status = m.data; scheduleRender(); }
    } else {
      try { const f = decodePreviewFrame(ev.data); state.images[f.gid] = f.dataUrl; scheduleRender(); } catch (e) { console.error(e); }
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
