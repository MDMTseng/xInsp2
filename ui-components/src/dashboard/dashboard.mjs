//
// dashboard.mjs — mountDashboard (task #81): drop the whole composable HMI
// dashboard into any host element. Renders a dashboard config ({title, layout})
// of cards (cards.mjs) via the layout engine (layout.mjs) and feeds them from an
// XiClient's live streams. RUN mode (read-only); the standalone HMI keeps its own
// compose editor. This is what lets an EXTERNAL webapp import the HMI dashboard —
// `import { mountDashboard } from "@xinsp2/components"`.
//
import { CARDS } from "./cards.mjs";
import { isLeaf, isSplit, isTabs, weightsOf } from "./layout.mjs";

export function mountDashboard(host, { client, dashboard, pollStatsMs = 200 }) {
  const doc = host.ownerDocument || globalThis.document;
  const raf = globalThis.requestAnimationFrame || ((cb) => setTimeout(cb, 16));
  const state = { run_id: -1, vars: {}, images: {}, run_ms: null, status: null, result: null, groups: [] };
  let cards = [], rafId = 0;

  function scheduleRender() {
    if (rafId) return;
    rafId = raf(() => { rafId = 0; for (const el of cards) { try { el.feed(state); } catch { /* card threw */ } } });
  }

  function renderCard(card) {
    const C = CARDS[card.type];
    const el = doc.createElement(C ? `xi-card-${card.type}` : "div");
    if (!C) { el.textContent = `unknown card: ${card.type}`; el.style.cssText = "color:#f88;padding:8px"; }
    el.binding = card.bind || {}; el.config = card.config || {};
    el.style.minWidth = "0"; el.style.minHeight = "0"; el.style.overflow = "hidden";
    if (C) cards.push(el);
    return el;
  }

  function renderTabs(node) {
    let active = Math.min(node.active || 0, node.tabs.length - 1);
    const wrap = doc.createElement("div");
    wrap.style.cssText = "display:flex;flex-direction:column;min-width:0;min-height:0;width:100%;height:100%";
    const strip = doc.createElement("div");
    strip.style.cssText = "display:flex;gap:2px;flex:0 0 auto;align-items:center;padding:2px 2px 0;overflow:auto";
    const bodyHost = doc.createElement("div");
    bodyHost.style.cssText = "flex:1 1 0;min-width:0;min-height:0;position:relative";
    const tabEls = [], bodies = [];
    const restyle = () => {
      tabEls.forEach((t, k) => { const on = k === active; t.style.background = on ? "#1e1e1e" : "#121212"; t.style.color = on ? "#ddd" : "#888"; });
      bodies.forEach((b, k) => { b.style.display = k === active ? "" : "none"; });
    };
    node.tabs.forEach((tb, i) => {
      const tab = doc.createElement("div");
      tab.style.cssText = "padding:4px 11px;cursor:pointer;border-radius:6px 6px 0 0;border:1px solid #333;border-bottom:none;font:12px system-ui,sans-serif";
      tab.textContent = tb.name || `Page ${i + 1}`;
      tab.onclick = () => { active = i; restyle(); };
      tabEls.push(tab); strip.appendChild(tab);
      const body = renderNode(tb.child);
      body.style.cssText += ";position:absolute;inset:0;min-width:0;min-height:0;border:1px solid #333;border-radius:0 6px 6px 6px;overflow:hidden";
      bodies.push(body); bodyHost.appendChild(body);
    });
    restyle();
    wrap.append(strip, bodyHost);
    return wrap;
  }

  function renderNode(node) {
    if (isLeaf(node)) return renderCard(node.card);
    if (isTabs(node)) return renderTabs(node);
    if (!isSplit(node)) { const e = doc.createElement("div"); e.textContent = "bad layout node"; e.style.color = "#f88"; return e; }
    const col = node.dir === "col";
    const box = doc.createElement("div");
    box.style.cssText = `display:flex;flex-direction:${col ? "column" : "row"};min-width:0;min-height:0;width:100%;height:100%`;
    const fr = weightsOf(node);
    node.children.forEach((c, i) => { const el = renderNode(c); el.style.flex = `${fr[i]} 1 0`; el.style.minWidth = "0"; el.style.minHeight = "0"; box.appendChild(el); });
    return box;
  }

  // Image cards only render once the backend streams their preview, and the
  // backend streams nothing unsubscribed. So subscribe to the var of every image
  // card in the current layout; re-diffed whenever the layout changes.
  let imgSubs = [];
  function collectImageVars(node, out) {
    if (!node) return out;
    if (isLeaf(node)) { const c = node.card; if (c && c.type === "image" && c.bind && c.bind.var) out.push(c.bind.var); }
    else if (isTabs(node)) (node.tabs || []).forEach((t) => collectImageVars(t.child || t, out));
    else if (isSplit(node)) (node.children || []).forEach((c) => collectImageVars(c, out));
    return out;
  }
  function updateImageSubs() {
    const next = collectImageVars(dashboard && dashboard.layout, []);
    for (const v of imgSubs) client.unsubscribeImage?.(v);
    for (const v of next) client.subscribeImage?.(v);
    imgSubs = next;
  }

  function render() {
    cards = [];
    host.replaceChildren();
    host.style.cssText += ";display:flex;min-width:0;min-height:0";
    updateImageSubs();
    const lay = dashboard && dashboard.layout;
    if (!lay) return;
    const el = renderNode(lay); el.style.flex = "1 1 0"; el.style.minWidth = "0"; el.style.minHeight = "0";
    host.appendChild(el);
    scheduleRender();
  }

  // Live streams (built on the shared XiClient — the HMI's own protocol/WS too).
  const offs = [
    client.onVars((v) => {
      state.run_id = v.run_id; state.vars = v.items;
      // Map each image var's gid → its canonical gid so a single deduped preview
      // frame resolves to the canon key the cards read from.
      const g2c = {};
      for (const name of Object.keys(v.items || {})) {
        const it = v.items[name];
        if (it && it.gid != null) g2c[it.gid] = it.src != null ? it.src : it.gid;
      }
      state.gidToCanon = g2c;
      scheduleRender();
    }),
    client.onPreview((f) => {
      const canon = state.gidToCanon && f.gid in state.gidToCanon ? state.gidToCanon[f.gid] : f.gid;
      // Shared decoded handle (decoded once in XiClient); dataUrl fallback.
      state.images[canon] = f.image || f.dataUrl;
      scheduleRender();
    }),
    client.onEvent((m) => {
      if (m.name === "run_finished" && m.data && typeof m.data.ms === "number") state.run_ms = m.data.ms;
      else if (m.name === "run_result" && m.data) { state.result = m.data; scheduleRender(); }
      else if (m.name === "safe_state" || m.name === "status") { state.status = m.data; scheduleRender(); }
    }),
  ];
  // Poll dispatch_stats for the groups card (cheap; ignored if no such card).
  const statsTimer = setInterval(() => {
    client.cmd("dispatch_stats").then((r) => { if (r && Array.isArray(r.groups)) { state.groups = r.groups; scheduleRender(); } }).catch(() => {});
  }, pollStatsMs);

  render();

  return {
    setDashboard(d) { dashboard = d; render(); },
    state,
    destroy() {
      offs.forEach((off) => off());
      for (const v of imgSubs) client.unsubscribeImage?.(v);
      imgSubs = [];
      clearInterval(statsTimer); host.replaceChildren();
    },
  };
}
