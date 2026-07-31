// viewport.mjs test — deterministic pan/zoom invariants (task #73), the same
// guarantees imageViewerPanel.ts's selftest checks, but on the pure module.
import { test } from "node:test";
import assert from "node:assert/strict";
import {
  createViewport, screenToImage, imageToScreen, fit, oneToOne, zoomAt, panBy,
  SCALE_MIN, SCALE_MAX,
} from "./viewport.mjs";

const near = (a, b, eps = 1e-9) => Math.abs(a - b) <= eps;

test("screen↔image round-trips", () => {
  const vp = Object.assign(createViewport(), { scale: 2.5, panX: 30, panY: -12 });
  const { x, y } = screenToImage(vp, 100, 80);
  const s = imageToScreen(vp, x, y);
  assert.ok(near(s.x, 100) && near(s.y, 80), "screen→image→screen is identity");
});

test("fit centers the image and keeps it inside the viewport", () => {
  const vp = Object.assign(createViewport(), { imgW: 640, imgH: 480, viewW: 800, viewH: 600 });
  fit(vp);
  // both image corners map inside [0,view]
  const tl = imageToScreen(vp, 0, 0);
  const br = imageToScreen(vp, vp.imgW, vp.imgH);
  assert.ok(tl.x >= 0 && tl.y >= 0 && br.x <= vp.viewW && br.y <= vp.viewH, "inside viewport");
  // centered: left margin == right margin
  assert.ok(near(tl.x, vp.viewW - br.x, 1e-6), "horizontally centered");
  assert.ok(near(tl.y, vp.viewH - br.y, 1e-6), "vertically centered");
});

test("cursor-anchored zoom keeps the point under the cursor fixed", () => {
  const vp = Object.assign(createViewport(), { imgW: 640, imgH: 480, viewW: 800, viewH: 600 });
  fit(vp);
  const cursor = { sx: 523, sy: 211 };
  const before = screenToImage(vp, cursor.sx, cursor.sy);
  zoomAt(vp, cursor.sx, cursor.sy, 1.15);
  zoomAt(vp, cursor.sx, cursor.sy, 1.15);
  const after = screenToImage(vp, cursor.sx, cursor.sy);
  assert.ok(near(before.x, after.x, 1e-6) && near(before.y, after.y, 1e-6),
    "the image point under the cursor is invariant across zoom");
});

test("zoom clamps to [SCALE_MIN, SCALE_MAX]", () => {
  const vp = Object.assign(createViewport(), { imgW: 10, imgH: 10, viewW: 100, viewH: 100, scale: 1 });
  for (let i = 0; i < 100; i++) zoomAt(vp, 50, 50, 2);
  assert.ok(vp.scale <= SCALE_MAX + 1e-9, "clamped at max");
  for (let i = 0; i < 200; i++) zoomAt(vp, 50, 50, 0.5);
  assert.ok(vp.scale >= SCALE_MIN - 1e-9, "clamped at min");
});

test("oneToOne sets scale 1 and centers", () => {
  const vp = Object.assign(createViewport(), { imgW: 200, imgH: 100, viewW: 800, viewH: 600 });
  oneToOne(vp);
  assert.equal(vp.scale, 1);
  assert.ok(near(imageToScreen(vp, 0, 0).x, 300) && near(imageToScreen(vp, 0, 0).y, 250), "centered at 1:1");
});

test("panBy shifts screen position by the delta", () => {
  const vp = Object.assign(createViewport(), { scale: 2, panX: 0, panY: 0 });
  const a = imageToScreen(vp, 10, 10);
  panBy(vp, 17, -3);
  const b = imageToScreen(vp, 10, 10);
  assert.ok(near(b.x - a.x, 17) && near(b.y - a.y, -3), "pan delta applied");
});
