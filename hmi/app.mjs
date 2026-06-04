//
// app.mjs — HMI host (v1, RUN mode): connect WS, decode the stream, lay out the
// dashboard grid, and feed each card. Compose/drag-drop + save_dashboard = v1.1.
//
import { parseVars, decodePreviewFrame } from "./protocol.mjs";
import { CARDS } from "./cards.mjs";
import { isLeaf, isSplit, clampRatio, validate } from "./layout.mjs";

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

// Recursively render a layout node into a filling element (RUN mode: dividers
// are fixed; drag-to-resize + split editing come with Compose mode in v1.1).
function renderNode(node) {
  if (isLeaf(node)) return renderCard(node.card);
  if (!isSplit(node)) { const e = document.createElement("div"); e.textContent = "bad layout node"; e.style.color = "#f88"; return e; }
  const box = document.createElement("div");
  box.style.cssText = `display:flex;flex-direction:${node.split === "col" ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%;gap:8px`;
  const r = clampRatio(node.ratio);
  const a = renderNode(node.a); a.style.flex = `${r} 1 0`;
  const b = renderNode(node.b); b.style.flex = `${1 - r} 1 0`;
  box.append(a, b);
  return box;
}

async function buildLayout() {
  let dash;
  try { dash = await (await fetch(DASH, { cache: "no-store" })).json(); }
  catch (e) { document.getElementById("err").textContent = `Could not load ${DASH}: ${e}`; return; }
  document.title = (dash.title || "xInsp2 HMI");
  const root = document.getElementById("grid");
  root.style.cssText += ";display:flex;min-width:0;min-height:0";
  cards = [];
  const problems = dash.layout ? validate(dash.layout) : ["dashboard has no 'layout' tree"];
  if (problems.length) { document.getElementById("err").textContent = problems.join("; "); }
  root.replaceChildren();
  if (dash.layout) { const el = renderNode(dash.layout); el.style.flex = "1 1 0"; root.appendChild(el); }
  scheduleRender();
}

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
