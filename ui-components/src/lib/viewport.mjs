//
// viewport.mjs — pure pan/zoom math for xi-image-viewer (task #73). Generalizes
// the transform in vscode-extension/src/imageViewerPanel.ts (and its selftest
// invariants) so it can be unit-tested without a DOM. The component is a thin
// canvas shell over these functions.
//
// Model: a screen point (sx,sy) maps to image space by ix = (sx - panX) / scale.
// All operations preserve that relationship.
//

export function createViewport() {
  return { scale: 1, panX: 0, panY: 0, imgW: 0, imgH: 0, viewW: 0, viewH: 0 };
}

export function screenToImage(vp, sx, sy) {
  return { x: (sx - vp.panX) / vp.scale, y: (sy - vp.panY) / vp.scale };
}

export function imageToScreen(vp, ix, iy) {
  return { x: vp.panX + ix * vp.scale, y: vp.panY + iy * vp.scale };
}

const SCALE_MIN = 0.05;
const SCALE_MAX = 64;
const clampScale = (s) => Math.max(SCALE_MIN, Math.min(SCALE_MAX, s));

// Fit the whole image in the viewport (95% margin) and center it.
export function fit(vp) {
  if (!vp.imgW || !vp.imgH || !vp.viewW || !vp.viewH) return vp;
  vp.scale = Math.min(vp.viewW / vp.imgW, vp.viewH / vp.imgH) * 0.95;
  vp.panX = (vp.viewW - vp.imgW * vp.scale) / 2;
  vp.panY = (vp.viewH - vp.imgH * vp.scale) / 2;
  return vp;
}

// 100% zoom, centered.
export function oneToOne(vp) {
  vp.scale = 1;
  vp.panX = (vp.viewW - vp.imgW) / 2;
  vp.panY = (vp.viewH - vp.imgH) / 2;
  return vp;
}

// Zoom by `factor` around screen point (sx,sy): the image point under the cursor
// stays under the cursor (cursor-anchored zoom).
export function zoomAt(vp, sx, sy, factor) {
  const { x: ix, y: iy } = screenToImage(vp, sx, sy);
  vp.scale = clampScale(vp.scale * factor);
  vp.panX = sx - ix * vp.scale;
  vp.panY = sy - iy * vp.scale;
  return vp;
}

// Pan by a screen-space delta.
export function panBy(vp, dx, dy) {
  vp.panX += dx;
  vp.panY += dy;
  return vp;
}

export { SCALE_MIN, SCALE_MAX, clampScale };
