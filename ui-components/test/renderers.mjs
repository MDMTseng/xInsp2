// renderers.mjs — the declarative renderer library v1 (doc 31). Pure compute
// cores get exact pixel/geometry/row assertions (canvas draw is browser-only, so
// the DOM tests assert element structure + the returned model, not pixels).
import { test } from "node:test";
import assert from "node:assert/strict";
import { setupDom } from "./_dom.mjs";
import { encodeCanonical } from "../src/canonical-mp.mjs";
import {
  readScalars, normalize, COLORMAPS, imageRGBA, heatmapRGBA,
  profilePoints, profileLength, overlayOps, tableRows, hexPreview,
  pickRenderer, renderDescriptor,
} from "../src/renderers.mjs";

const w = setupDom();

// ---- dtype reads -------------------------------------------------------------
test("readScalars reads f32 / u16 little-endian, fail-loud on short/unknown", () => {
  assert.deepEqual([...readScalars(new Float32Array([1.5, -2, 4]).buffer, "f32", 3)], [1.5, -2, 4]);
  assert.deepEqual([...readScalars(new Uint16Array([0, 258, 65535]).buffer, "u16", 3)], [0, 258, 65535]);
  assert.throws(() => readScalars(new Uint8Array(2).buffer, "f32", 1), /shorter/);
  assert.throws(() => readScalars(new Uint8Array(8).buffer, "q7", 1), /unsupported dt/);
});

// ---- normalize ---------------------------------------------------------------
test("normalize maps to [0,1] over range or the data extent; flat → 0", () => {
  assert.deepEqual([...normalize([0, 5, 10]).norm], [0, 0.5, 1]);
  assert.deepEqual([...normalize([0, 5, 10], [0, 20]).norm], [0, 0.25, 0.5]);
  assert.deepEqual([...normalize([7, 7, 7]).norm], [0, 0, 0]);   // zero span
  const n = normalize([2, 4]); assert.equal(n.min, 2); assert.equal(n.max, 4);
});

// ---- colormaps ---------------------------------------------------------------
test("colormaps hit their endpoints exactly", () => {
  assert.deepEqual(COLORMAPS.gray(0), [0, 0, 0]);
  assert.deepEqual(COLORMAPS.gray(1), [255, 255, 255]);
  assert.deepEqual(COLORMAPS.viridis(0), [68, 1, 84]);
  assert.deepEqual(COLORMAPS.viridis(1), [253, 231, 37]);
});

// ---- image -------------------------------------------------------------------
test("imageRGBA expands gray→RGBA and passes rgb through", () => {
  assert.deepEqual([...imageRGBA(new Uint8Array([10, 250]), 2, 1, 1)],
    [10, 10, 10, 255, 250, 250, 250, 255]);
  assert.deepEqual([...imageRGBA(new Uint8Array([1, 2, 3]), 1, 1, 3)], [1, 2, 3, 255]);
  assert.throws(() => imageRGBA(new Uint8Array([1]), 2, 1, 1), /shorter/);
});

// ---- heatmap -----------------------------------------------------------------
test("heatmapRGBA colormaps normalized scalars; reports min/max", () => {
  const { rgba, min, max } = heatmapRGBA(new Float32Array([0, 1]).buffer, 2, 1, "f32", { colormap: "gray" });
  assert.deepEqual([...rgba], [0, 0, 0, 255, 255, 255, 255, 255]);
  assert.equal(min, 0); assert.equal(max, 1);
});

// ---- profile -----------------------------------------------------------------
test("profilePoints spreads x across width, inverts y; profileLength reads shape", () => {
  const pts = profilePoints([0, 1, 0.5], { width: 100, height: 100, range: [0, 1], pad: 2 });
  assert.deepEqual(pts, [[2, 98], [50, 2], [98, 50]]);
  assert.equal(profileLength({ w: 5, h: 1 }), 5);
  assert.equal(profileLength({ w: 1, h: 7 }), 7);
  assert.equal(profileLength({ n: 9 }), 9);
});

// ---- overlay -----------------------------------------------------------------
test("overlayOps scales shapes from descriptor space to the viewport", () => {
  const ops = overlayOps(
    [{ type: "point", x: 5, y: 5 }, { type: "box", x: 0, y: 0, w: 10, h: 10 },
     { type: "polyline", points: [[0, 0], [10, 10]] }],
    { w: 10, h: 10, vw: 20, vh: 20 });
  assert.deepEqual(ops[0], { type: "point", x: 10, y: 10, r: 3, color: "#39f" });
  assert.deepEqual(ops[1], { type: "box", x: 0, y: 0, w: 20, h: 20, color: "#39f" });
  assert.deepEqual(ops[2].points, [[0, 0], [20, 20]]);
});

// ---- table + hex -------------------------------------------------------------
test("tableRows flattens scalars, JSON-stringifies nested; hexPreview formats bytes", () => {
  assert.deepEqual(tableRows({ a: 1, b: "x", c: { n: 2 } }), [["a", "1"], ["b", "x"], ["c", '{"n":2}']]);
  assert.equal(hexPreview(new Uint8Array([0xde, 0xad, 0xbe, 0xef]), 3), "de ad be …");
});

// ---- dispatch ----------------------------------------------------------------
test("pickRenderer routes by render hint, then t + dt + shape", () => {
  assert.equal(pickRenderer({ t: "xi/image", dt: "u8" }), "image");
  assert.equal(pickRenderer({ t: "xi/image", dt: "f32", w: 8, h: 8 }), "heatmap");
  assert.equal(pickRenderer({ t: "xi/image", dt: "f32", w: 16, h: 1 }), "profile");
  assert.equal(pickRenderer({ render: "table" }), "table");
  assert.equal(pickRenderer({ render: "overlay" }), "overlay");
  assert.equal(pickRenderer({ t: "acme/thing" }), "hex");
});

// ---- renderDescriptor into a DOM host (structure + returned model) -----------
test("renderDescriptor: image → a canvas of the right size", () => {
  const host = w.document.createElement("div");
  const r = renderDescriptor(host, { desc: { t: "xi/image", dt: "u8", w: 2, h: 1, c: 1 }, payload: new Uint8Array([9, 9]) });
  assert.equal(r.kind, "image");
  const cv = host.querySelector("canvas");
  assert.ok(cv && cv.width === 2 && cv.height === 1, "canvas 2×1 appended");
});

test("renderDescriptor: table → a <table> with a row per key (msgpack payload)", () => {
  const host = w.document.createElement("div");
  const payload = encodeCanonical({ area: 42, ok: true });
  const r = renderDescriptor(host, { desc: { render: "table" }, payload });
  assert.deepEqual(r.rows, [["area", "42"], ["ok", "true"]]);
  assert.equal(host.querySelectorAll("table.xi-render-table tr").length, 2);
});

test("renderDescriptor: unknown t → a hex fallback card with type + size", () => {
  const host = w.document.createElement("div");
  const r = renderDescriptor(host, { desc: { t: "acme/blob" }, payload: new Uint8Array([1, 2, 3, 4]) });
  assert.equal(r.kind, "hex");
  assert.equal(r.type, "acme/blob");
  assert.equal(r.size, 4);
  assert.equal(host.querySelector(".xi-hex-type").textContent, "acme/blob");
});
