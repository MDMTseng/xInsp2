//
// cards.mjs — built-in HMI cards as web components (xInsp2 production HMI v1).
//
// Contract (see docs/design/production-hmi.md):
//   - host sets  el.binding = { var: "name", source? }  and  el.config = {...}
//   - host calls el.feed({ run_id, vars, images, run_ms, status }) on each update
//   - cards never open their own WS; they only render what they're fed.
// state.vars is a name->item map; state.images is a gid->dataUrl map.
//

const css = `
  :host { display:block; height:100%; box-sizing:border-box; background:#1e1e1e;
          border:1px solid #333; border-radius:6px; color:#ddd; overflow:hidden;
          font:13px/1.4 system-ui,sans-serif; }
  .hd { padding:4px 8px; font-size:11px; letter-spacing:.04em; text-transform:uppercase;
        color:#888; border-bottom:1px solid #2a2a2a; }
  .body { padding:8px; height:calc(100% - 24px); box-sizing:border-box; }
`;
function shell(el, title) {
  el.attachShadow({ mode: "open" });
  el.shadowRoot.innerHTML = `<style>${css}</style>
    <div class="hd">${title || ""}</div><div class="body"></div>`;
  return el.shadowRoot.querySelector(".body");
}
const titleOf = (el, fallback) => (el.config && el.config.title) || (el.binding && el.binding.var) || fallback;
const val = (st, b) => (b && st.vars[b.var] ? st.vars[b.var].value : undefined);

// ---- verdict: big OK/NG tile ------------------------------------------------
class VerdictCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, titleOf(this, "Verdict"));
    this.body.style.cssText = "display:flex;align-items:center;justify-content:center;font-weight:800;font-size:clamp(20px,7vw,72px)"; }
  feed(st) {
    const v = val(st, this.binding);
    const ok = v === true || v === "OK" || v === "ok" || v === "PASS";
    const ng = v === false || v === "NG" || v === "ng" || v === "FAIL";
    this.body.textContent = v === undefined ? "—" : (ok ? "OK" : ng ? "NG" : String(v));
    this.body.style.color = ok ? "#3ad17a" : ng ? "#ff5b5b" : "#ccc";
  }
}

// ---- value: single readout --------------------------------------------------
class ValueCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, titleOf(this, "Value"));
    this.body.style.cssText = "display:flex;align-items:center;justify-content:center;font-size:clamp(16px,5vw,40px);font-weight:600"; }
  feed(st) { const v = val(st, this.binding);
    this.body.textContent = v === undefined ? "—" : (typeof v === "number" ? (+v.toFixed(this.config?.decimals ?? 3)) : String(v)); }
}

// ---- image: base output image (overlays = v1.1) -----------------------------
class ImageCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, titleOf(this, "Image"));
    this.body.style.cssText = "display:flex;align-items:center;justify-content:center;padding:2px";
    this.img = document.createElement("img");
    this.img.style.cssText = `max-width:100%;max-height:100%;object-fit:${this.config?.fit || "contain"};image-rendering:pixelated`;
    this.body.appendChild(this.img); }
  feed(st) { const it = this.binding && st.vars[this.binding.var];
    const url = it && it.gid != null ? st.images[it.gid] : undefined;
    if (url && url !== this._u) { this.img.src = url; this._u = url; } }
}

// ---- spc: rolling trend + control lines -------------------------------------
class SpcCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, titleOf(this, "SPC")); this.buf = []; this.last = -1;
    this.cv = document.createElement("canvas"); this.cv.style.cssText = "width:100%;height:100%";
    this.body.appendChild(this.cv); }
  feed(st) {
    if (st.run_id !== this.last) { this.last = st.run_id;
      const v = val(st, this.binding);
      if (typeof v === "number") { this.buf.push(v); const w = this.config?.window || 100; if (this.buf.length > w) this.buf.shift(); }
    }
    this.draw();
  }
  draw() {
    const c = this.cv, b = c.getBoundingClientRect(); if (!b.width) return;
    c.width = b.width; c.height = b.height; const g = c.getContext("2d");
    g.clearRect(0, 0, c.width, c.height);
    if (!this.buf.length) return;
    const mean = this.config?.mean ?? this.buf.reduce((a, x) => a + x, 0) / this.buf.length;
    const ucl = this.config?.ucl, lcl = this.config?.lcl;
    let lo = Math.min(...this.buf), hi = Math.max(...this.buf);
    if (ucl != null) hi = Math.max(hi, ucl); if (lcl != null) lo = Math.min(lo, lcl);
    const pad = (hi - lo) * 0.1 || 1; lo -= pad; hi += pad;
    const Y = (v) => c.height - ((v - lo) / (hi - lo)) * c.height;
    const line = (v, col, dash) => { if (v == null) return; g.strokeStyle = col; g.setLineDash(dash || []);
      g.beginPath(); g.moveTo(0, Y(v)); g.lineTo(c.width, Y(v)); g.stroke(); g.setLineDash([]); };
    line(mean, "#666"); line(ucl, "#ff5b5b", [4, 3]); line(lcl, "#ff5b5b", [4, 3]);
    g.strokeStyle = "#4aa0f0"; g.lineWidth = 1.5; g.beginPath();
    this.buf.forEach((v, i) => { const x = (i / Math.max(1, this.buf.length - 1)) * c.width;
      i ? g.lineTo(x, Y(v)) : g.moveTo(x, Y(v)); }); g.stroke();
  }
}

// ---- throughput: parts/min + cycle time -------------------------------------
class ThroughputCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, this.config?.title || "Throughput"); this.buf = []; this.last = -1;
    this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px";
    this.big = document.createElement("div"); this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700;color:#9ad";
    this.sub = document.createElement("div"); this.sub.style.cssText = "font-size:12px;color:#888";
    this.body.append(this.big, this.sub); }
  feed(st) {
    if (st.run_id !== this.last && st.run_ms != null) { this.last = st.run_id;
      this.buf.push(st.run_ms); if (this.buf.length > 30) this.buf.shift(); }
    if (this.buf.length) { const avg = this.buf.reduce((a, x) => a + x, 0) / this.buf.length;
      this.big.textContent = `${(60000 / avg).toFixed(0)} /min`;
      this.sub.textContent = `cycle ${avg.toFixed(1)} ms`; }
  }
}

// ---- yield: OK/NG counts + pass-rate % --------------------------------------
class YieldCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, this.config?.title || "Yield"); this.ok = 0; this.ng = 0; this.last = -1;
    this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4px";
    this.big = document.createElement("div"); this.big.style.cssText = "font-size:clamp(18px,6vw,44px);font-weight:700";
    this.sub = document.createElement("div"); this.sub.style.cssText = "font-size:12px;color:#888";
    this.body.append(this.big, this.sub); }
  feed(st) {
    if (st.run_id !== this.last) { this.last = st.run_id; const v = val(st, this.binding);
      if (v !== undefined) { const ok = v === true || v === "OK" || v === "ok" || v === "PASS"; ok ? this.ok++ : this.ng++; } }
    const n = this.ok + this.ng, pct = n ? (100 * this.ok / n) : 0;
    this.big.textContent = `${pct.toFixed(1)}%`; this.big.style.color = pct >= (this.config?.warn ?? 95) ? "#3ad17a" : "#ffb454";
    this.sub.textContent = `OK ${this.ok} / NG ${this.ng}`;
  }
}

export const CARDS = {
  verdict: VerdictCard, value: ValueCard, image: ImageCard,
  spc: SpcCard, throughput: ThroughputCard, yield: YieldCard,
};
for (const [k, C] of Object.entries(CARDS)) customElements.define(`xi-card-${k}`, C);
