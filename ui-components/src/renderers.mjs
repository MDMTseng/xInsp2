//
// renderers.mjs — the declarative renderer library v1 (doc 31 "no-code path").
// A self-describing blob's DESCRIPTOR (its canonical-msgpack map: `t`, `w`, `h`,
// `c`, `dt`, plus an optional `render` hint and per-renderer keys) drives which
// renderer draws its payload — no plugin-specific UI code. Small, dependency-free,
// canvas-based; every renderer splits a PURE compute core (unit-tested with fixed
// inputs → exact pixel/geometry/row assertions) from a thin draw step (browser
// canvas / DOM; a no-op where a 2d context is unavailable, e.g. jsdom).
//
// Renderers (keyed by descriptor `render` hint, else inferred from `t` + `dt`):
//   image    xi/image u8               → RGBA canvas
//   heatmap  f32/u16/f64 2-D scalars   → colormapped canvas (range + colormap)
//   profile  1×N / N×1 scalars         → polyline chart
//   overlay  shapes over an image ref  → points/boxes/polylines
//   table    a msgpack map / object    → key/value rows
//   hex      anything else (fallback)  → a card: type + byte size + hex preview
//
// See ui-components/README.md "Declarative renderer vocabulary v1".
//
import { decode } from "./canonical-mp.mjs";

// ---- dtype element readers (little-endian, matching the pool payload) --------
export const DTYPE = {
  u8:  { size: 1, read: (dv, o) => dv.getUint8(o) },
  u16: { size: 2, read: (dv, o) => dv.getUint16(o, true) },
  i32: { size: 4, read: (dv, o) => dv.getInt32(o, true) },
  f32: { size: 4, read: (dv, o) => dv.getFloat32(o, true) },
  f64: { size: 8, read: (dv, o) => dv.getFloat64(o, true) },
};

function asDataView(payload) {
  if (payload instanceof DataView) return payload;
  if (payload instanceof ArrayBuffer) return new DataView(payload);
  if (ArrayBuffer.isView(payload)) return new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  throw new Error("payload must be an ArrayBuffer / TypedArray / DataView");
}

// Read `count` dtype scalars from `payload` into a Float64Array. Fail-loud on an
// unknown dtype or a payload too short for count*elem_size.
export function readScalars(payload, dt, count) {
  const t = DTYPE[dt];
  if (!t) throw new Error(`readScalars: unsupported dt "${dt}"`);
  const dv = asDataView(payload);
  if (dv.byteLength < count * t.size) throw new Error("readScalars: payload shorter than count*elem_size");
  const out = new Float64Array(count);
  for (let i = 0; i < count; i++) out[i] = t.read(dv, i * t.size);
  return out;
}

// ---- normalization -----------------------------------------------------------
// Map values to [0,1] over `range` (or the data's own [min,max]). A zero-width
// range maps everything to 0 (avoids divide-by-zero, deterministic).
export function normalize(values, range) {
  let min, max;
  if (range && range.length === 2) { min = range[0]; max = range[1]; }
  else {
    min = Infinity; max = -Infinity;
    for (const v of values) { if (v < min) min = v; if (v > max) max = v; }
    if (!isFinite(min)) { min = 0; max = 0; }
  }
  const span = max - min;
  const norm = new Float64Array(values.length);
  for (let i = 0; i < values.length; i++) {
    norm[i] = span > 0 ? Math.min(1, Math.max(0, (values[i] - min) / span)) : 0;
  }
  return { min, max, norm };
}

// ---- colormaps: t∈[0,1] → [r,g,b] (0..255) -----------------------------------
function lerp(a, b, u) { return a + (b - a) * u; }
function stops(table, t) {
  const u = Math.min(1, Math.max(0, t)) * (table.length - 1);
  const i = Math.floor(u), f = u - i;
  const a = table[i], b = table[Math.min(table.length - 1, i + 1)];
  return [Math.round(lerp(a[0], b[0], f)), Math.round(lerp(a[1], b[1], f)), Math.round(lerp(a[2], b[2], f))];
}
const VIRIDIS = [[68,1,84],[72,40,120],[62,74,137],[49,104,142],[38,130,142],
  [31,158,137],[53,183,121],[110,206,88],[181,222,43],[253,231,37]];
const JET = [[0,0,131],[0,60,170],[5,255,255],[255,255,0],[250,0,0],[128,0,0]];
export const COLORMAPS = {
  gray:    (t) => { const v = Math.round(Math.min(1, Math.max(0, t)) * 255); return [v, v, v]; },
  viridis: (t) => stops(VIRIDIS, t),
  jet:     (t) => stops(JET, t),
};

// ============================================================================
// IMAGE — xi/image u8 → RGBA (gray → gray, 3ch → RGB, 4ch → RGBA passthrough).
// ============================================================================
export function imageRGBA(payload, w, h, c) {
  const dv = asDataView(payload);
  if (dv.byteLength < w * h * c) throw new Error("imageRGBA: payload shorter than w*h*c");
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let p = 0; p < w * h; p++) {
    const s = p * c, d = p * 4;
    if (c === 1) { const v = dv.getUint8(s); rgba[d] = rgba[d + 1] = rgba[d + 2] = v; rgba[d + 3] = 255; }
    else if (c === 2) { const v = dv.getUint8(s); rgba[d] = rgba[d + 1] = rgba[d + 2] = v; rgba[d + 3] = dv.getUint8(s + 1); }
    else { rgba[d] = dv.getUint8(s); rgba[d + 1] = dv.getUint8(s + 1); rgba[d + 2] = dv.getUint8(s + 2); rgba[d + 3] = c >= 4 ? dv.getUint8(s + 3) : 255; }
  }
  return rgba;
}

// ============================================================================
// HEATMAP — f32/u16/f64 2-D scalars → colormapped RGBA over a range.
// ============================================================================
export function heatmapRGBA(payload, w, h, dt, { range, colormap = "viridis" } = {}) {
  const values = readScalars(payload, dt, w * h);
  const { norm, min, max } = normalize(values, range);
  const cmap = COLORMAPS[colormap] || COLORMAPS.viridis;
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const [r, g, b] = cmap(norm[i]);
    const d = i * 4; rgba[d] = r; rgba[d + 1] = g; rgba[d + 2] = b; rgba[d + 3] = 255;
  }
  return { rgba, min, max };
}

// ============================================================================
// PROFILE — a 1×N / N×1 vector → polyline points in a [width×height] chart box
// (y inverted so larger values sit higher). Values normalize over `range` or
// their own extent. Padding keeps the trace off the edges.
// ============================================================================
export function profilePoints(values, { width, height, range, pad = 2 } = {}) {
  const n = values.length;
  const { norm } = normalize(values, range);
  const iw = Math.max(1, width - 2 * pad), ih = Math.max(1, height - 2 * pad);
  const pts = new Array(n);
  for (let i = 0; i < n; i++) {
    const x = pad + (n === 1 ? iw / 2 : (i / (n - 1)) * iw);
    const y = pad + (1 - norm[i]) * ih;      // invert: 1 → top
    pts[i] = [x, y];
  }
  return pts;
}

// Infer the vector length + orientation of a profile descriptor (1×N or N×1).
export function profileLength(desc) {
  const w = desc.w | 0, h = desc.h | 0;
  if (desc.n) return desc.n | 0;
  if (h === 1) return w;
  if (w === 1) return h;
  return Math.max(w, h);   // tolerate a non-degenerate shape (row-major first row)
}

// ============================================================================
// OVERLAY — normalize declarative shapes into draw ops, scaled from descriptor
// space (w×h) to a viewport (vw×vh). Shapes:
//   { type:"point",   x, y, r?, color? }
//   { type:"box",     x, y, w, h, color? }        (x,y = top-left)
//   { type:"polyline", points:[[x,y],…], color?, closed? }
// Returns [{ type, ...scaled coords, color }] — geometry a caller strokes.
// ============================================================================
export function overlayOps(shapes, { w = 1, h = 1, vw = w, vh = h } = {}) {
  const sx = vw / w, sy = vh / h;
  const P = (x, y) => [x * sx, y * sy];
  const out = [];
  for (const s of shapes || []) {
    const color = s.color || "#39f";
    if (s.type === "point") { const [x, y] = P(s.x, s.y); out.push({ type: "point", x, y, r: (s.r || 3), color }); }
    else if (s.type === "box") { const [x, y] = P(s.x, s.y); out.push({ type: "box", x, y, w: s.w * sx, h: s.h * sy, color }); }
    else if (s.type === "polyline") { out.push({ type: "polyline", points: (s.points || []).map(([x, y]) => P(x, y)), closed: !!s.closed, color }); }
  }
  return out;
}

// ============================================================================
// TABLE — a decoded msgpack map / plain object → [[key, valueString], …] rows.
// Nested objects/arrays are JSON-stringified (compact); scalars shown as-is.
// ============================================================================
export function tableRows(value) {
  const obj = (value && typeof value === "object" && !Array.isArray(value)) ? value
            : (value instanceof Map ? Object.fromEntries(value) : { value });
  const fmt = (v) => (v === null || typeof v !== "object") ? String(v) : JSON.stringify(v);
  return Object.entries(obj).map(([k, v]) => [k, fmt(v)]);
}

// ============================================================================
// HEX — fallback: a { type, size, preview } card model for an unknown blob.
// ============================================================================
export function hexPreview(payload, n = 16) {
  const dv = asDataView(payload);
  const m = Math.min(n, dv.byteLength);
  const parts = [];
  for (let i = 0; i < m; i++) parts.push(dv.getUint8(i).toString(16).padStart(2, "0"));
  return parts.join(" ") + (dv.byteLength > m ? " …" : "");
}
export function hexCard(desc, payload) {
  const size = payload ? asDataView(payload).byteLength : 0;
  return { type: (desc && desc.t) || "unknown", size, preview: payload ? hexPreview(payload) : "" };
}

// ============================================================================
// DISPATCH — pick a renderer id from the descriptor (explicit `render` wins,
// else inferred from `t` + `dt` + shape).
// ============================================================================
export function pickRenderer(desc = {}) {
  if (desc.render && RENDERERS[desc.render]) return desc.render;
  if (desc.render === "table") return "table";
  const t = desc.t, dt = desc.dt;
  if (t === "xi/image") {
    if (dt === "u8") return "image";
    if (dt === "f32" || dt === "u16" || dt === "f64") {
      return (desc.h === 1 || desc.w === 1) ? "profile" : "heatmap";
    }
  }
  return "hex";
}

// ---- draw glue: create a canvas + putImageData (no-op if no 2d context) ------
function makeCanvas(host, w, h) {
  const doc = host.ownerDocument || globalThis.document;
  const cv = doc.createElement("canvas");
  cv.width = w; cv.height = h;
  host.innerHTML = ""; host.appendChild(cv);
  return cv;
}
function paintRGBA(cv, rgba, w, h) {
  const ctx = cv.getContext && cv.getContext("2d");
  if (!ctx || !ctx.putImageData) return false;   // jsdom / no-canvas: compute-only
  const doc = cv.ownerDocument || globalThis.document;
  const img = (ctx.createImageData ? ctx.createImageData(w, h) : new (doc.defaultView.ImageData)(w, h));
  img.data.set(rgba);
  ctx.putImageData(img, 0, 0);
  return true;
}

// The renderer table: each takes (host, { desc, payload, refs? }) and draws.
export const RENDERERS = {
  image(host, { desc, payload }) {
    const { w, h, c = 1 } = desc;
    const rgba = imageRGBA(payload, w, h, c);
    paintRGBA(makeCanvas(host, w, h), rgba, w, h);
    return { kind: "image", w, h, c };
  },
  heatmap(host, { desc, payload }) {
    const { w, h, dt = "f32" } = desc;
    const { rgba, min, max } = heatmapRGBA(payload, w, h, dt, { range: desc.range, colormap: desc.colormap });
    paintRGBA(makeCanvas(host, w, h), rgba, w, h);
    return { kind: "heatmap", w, h, min, max, colormap: desc.colormap || "viridis" };
  },
  profile(host, { desc, payload }) {
    const n = profileLength(desc);
    const values = readScalars(payload, desc.dt || "f32", n);
    const width = desc.width || 240, height = desc.height || 80;
    const pts = profilePoints(values, { width, height, range: desc.range });
    const cv = makeCanvas(host, width, height);
    const ctx = cv.getContext && cv.getContext("2d");
    if (ctx && ctx.beginPath) {
      ctx.strokeStyle = desc.color || "#39f"; ctx.beginPath();
      pts.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
      ctx.stroke();
    }
    return { kind: "profile", n, points: pts };
  },
  overlay(host, { desc, refs = {} }) {
    const { w = 1, h = 1 } = desc;
    const vw = desc.width || w, vh = desc.height || h;
    const ops = overlayOps(desc.shapes, { w, h, vw, vh });
    const cv = makeCanvas(host, vw, vh);
    const ctx = cv.getContext && cv.getContext("2d");
    if (ctx && ctx.beginPath) {
      const bg = desc.image && refs[desc.image];   // an ImageBitmap/Canvas the caller supplied
      if (bg && ctx.drawImage) ctx.drawImage(bg, 0, 0, vw, vh);
      for (const op of ops) {
        ctx.strokeStyle = op.color; ctx.fillStyle = op.color; ctx.beginPath();
        if (op.type === "point") { ctx.arc(op.x, op.y, op.r, 0, Math.PI * 2); ctx.fill(); }
        else if (op.type === "box") { ctx.strokeRect(op.x, op.y, op.w, op.h); }
        else if (op.type === "polyline") {
          op.points.forEach(([x, y], i) => (i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)));
          if (op.closed) ctx.closePath();
          ctx.stroke();
        }
      }
    }
    return { kind: "overlay", ops };
  },
  table(host, { desc, payload }) {
    // The map is the payload (msgpack) if present, else the descriptor's own body.
    let value = desc.value;
    if (payload) { try { value = decode(payload instanceof Uint8Array ? payload : new Uint8Array(asDataView(payload).buffer)); } catch { /* keep desc.value */ } }
    const rows = tableRows(value != null ? value : desc);
    const doc = host.ownerDocument || globalThis.document;
    host.innerHTML = "";
    const tbl = doc.createElement("table"); tbl.className = "xi-render-table";
    for (const [k, v] of rows) {
      const tr = doc.createElement("tr");
      const th = doc.createElement("th"); th.textContent = k;
      const td = doc.createElement("td"); td.textContent = v;
      tr.appendChild(th); tr.appendChild(td); tbl.appendChild(tr);
    }
    host.appendChild(tbl);
    return { kind: "table", rows };
  },
  hex(host, { desc, payload }) {
    const card = hexCard(desc, payload);
    const doc = host.ownerDocument || globalThis.document;
    host.innerHTML = "";
    const el = doc.createElement("div"); el.className = "xi-render-hex";
    const title = doc.createElement("div"); title.className = "xi-hex-type"; title.textContent = card.type;
    const meta = doc.createElement("div"); meta.className = "xi-hex-size"; meta.textContent = `${card.size} bytes`;
    const pre = doc.createElement("code"); pre.className = "xi-hex-preview"; pre.textContent = card.preview;
    el.appendChild(title); el.appendChild(meta); el.appendChild(pre);
    host.appendChild(el);
    return { kind: "hex", ...card };
  },
};

// The one entry point: render a self-describing blob model into `host`.
// model: { desc, payload?, refs? }. Returns the chosen renderer's result model.
export function renderDescriptor(host, model) {
  const id = pickRenderer(model.desc || {});
  return RENDERERS[id](host, model);
}
