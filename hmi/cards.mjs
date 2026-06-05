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

// Interpret a run_result code into a verdict bucket (see docs/design/run-result.md):
// >0 ok-class, 0 NA, -1..-989999 ng-class, <=-990000 framework system-fail.
function classifyResult(code) {
  if (code == null)        return { kind: "none", label: "—",  color: "#bbb"  };
  if (code <= -990000)     return { kind: "sys",  label: "SYS", color: "#c084fc" };
  if (code > 0)            return { kind: "ok",   label: code > 1 ? `OK${code}` : "OK", color: "#3ad17a" };
  if (code < 0)            return { kind: "ng",   label: code < -1 ? `NG${-code}` : "NG", color: "#ff5b5b" };
  return                          { kind: "na",   label: "NA",  color: "#ffb454" };  // 0
}
// A card consumes the run_result stream when bound `{result:true}` or left unbound.
const usesResult = (b) => !b || b.result === true || !b.var;

// ---- verdict: big OK/NG tile ------------------------------------------------
class VerdictCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, titleOf(this, "Verdict"));
    this.body.style.cssText = "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:6px;text-align:center";
    this.big = document.createElement("div"); this.big.style.cssText = "font-weight:800;font-size:clamp(20px,7vw,72px);line-height:1";
    this.sub = document.createElement("div"); this.sub.style.cssText = "font-size:12px;color:#888;max-width:96%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap";
    this.body.append(this.big, this.sub); }
  feed(st) {
    const b = this.binding || {};
    if (usesResult(b)) {                       // run_result mode (the per-run verdict)
      const r = st.result, c = classifyResult(r ? r.code : null);
      this.big.textContent = c.label; this.big.style.color = c.color;
      this.sub.textContent = r && r.msg ? r.msg : "";
      return;
    }
    const v = val(st, b);                       // legacy: bound to a bool/string var
    const ok = v === true || v === "OK" || v === "ok" || v === "PASS";
    const ng = v === false || v === "NG" || v === "ng" || v === "FAIL";
    this.big.textContent = v === undefined ? "—" : (ok ? "OK" : ng ? "NG" : String(v));
    this.big.style.color = ok ? "#3ad17a" : ng ? "#ff5b5b" : "#ccc";
    this.sub.textContent = "";
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
    const b = this.binding || {};
    if (usesResult(b)) {                        // count from run_result (inspected runs)
      const r = st.result;
      if (r && r.run_id != null && r.run_id !== this.last) { this.last = r.run_id;
        const c = classifyResult(r.code);
        if (c.kind === "ok") this.ok++; else if (c.kind === "ng") this.ng++;
        else if (c.kind === "na") this.na = (this.na || 0) + 1; }
    } else if (st.run_id !== this.last) {        // legacy: bound to a bool/string var
      this.last = st.run_id; const v = val(st, b);
      if (v !== undefined) { const ok = v === true || v === "OK" || v === "ok" || v === "PASS"; ok ? this.ok++ : this.ng++; } }
    const n = this.ok + this.ng, pct = n ? (100 * this.ok / n) : 0;
    this.big.textContent = `${pct.toFixed(1)}%`; this.big.style.color = pct >= (this.config?.warn ?? 95) ? "#3ad17a" : "#ffb454";
    this.sub.textContent = `OK ${this.ok} / NG ${this.ng}` + (this.na ? ` · NA ${this.na}` : "");
  }
}

// ---- groups: live dispatch-group concurrency monitor ------------------------
// Reads st.groups (the dispatch_stats breakdown the host polls). One row per
// group: a bar of `max_parallel` cells with `running` lit, plus queue / dropped
// / peak. Makes per-group parallelism visible (each lane fills to its cap).
class GroupsCard extends HTMLElement {
  connectedCallback() { this.body = shell(this, this.config?.title || "Dispatch groups");
    this.body.style.cssText = "display:flex;flex-direction:column;gap:10px;padding:10px;overflow:auto";
    this.peak = {}; this.rows = {}; }
  feed(st) {
    const groups = st.groups || [];
    if (!groups.length) { this.body.textContent = "no dispatch groups (legacy single pool)"; this.body.style.color = "#888"; return; }
    for (const g of groups) {
      const mp = g.max_parallel || 1, run = g.running || 0;
      this.peak[g.name] = Math.max(this.peak[g.name] || 0, run);
      let row = this.rows[g.name];
      if (!row) {
        row = document.createElement("div"); row.style.cssText = "display:flex;flex-direction:column;gap:3px";
        const head = document.createElement("div"); head.style.cssText = "display:flex;justify-content:space-between;font-size:12px";
        const name = document.createElement("span"); name.style.fontWeight = "600";
        const meta = document.createElement("span"); meta.style.color = "#888";
        head.append(name, meta);
        const bar = document.createElement("div"); bar.style.cssText = "display:flex;gap:3px;height:18px";
        row.append(head, bar); this.body.appendChild(row);
        this.rows[g.name] = row = { row, name, meta, bar, cells: [] };
      }
      row.name.textContent = `${g.name}  ${run}/${mp}`;
      row.name.style.color = run >= mp ? "#3ad17a" : run > 0 ? "#9ad" : "#bbb";
      row.meta.textContent = `q ${g.queue_now ?? 0} · drop ${g.dropped ?? 0} · peak ${this.peak[g.name]}`;
      if (row.cells.length !== mp) {                  // (re)build the cell strip
        row.bar.replaceChildren(); row.cells = [];
        for (let i = 0; i < mp; i++) { const c = document.createElement("div");
          c.style.cssText = "flex:1 1 0;border-radius:3px;border:1px solid #333;min-width:6px"; row.bar.appendChild(c); row.cells.push(c); }
      }
      row.cells.forEach((c, i) => { c.style.background = i < run ? "#3ad17a" : "#1a1a1a"; });
    }
  }
}

export const CARDS = {
  verdict: VerdictCard, value: ValueCard, image: ImageCard,
  spc: SpcCard, throughput: ThroughputCard, yield: YieldCard, groups: GroupsCard,
};
for (const [k, C] of Object.entries(CARDS)) customElements.define(`xi-card-${k}`, C);
