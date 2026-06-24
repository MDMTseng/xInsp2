<svelte:options customElement={{ tag: "xi-image-viewer" }} />

<script>
  // Canvas image viewer: wheel = cursor-anchored zoom, drag = pan, click = pixel
  // probe. The pan/zoom math lives in ../lib/viewport.mjs (unit-tested). Tap-out
  // API attached to the host element: setFrame(src) / fit() / oneToOne(); events
  // `pixelpick` ({x,y,rgb}) and `viewchange` ({scale}). The host wires
  // XiClient.onPreview → viewer.setFrame(frame.dataUrl) — the component stays
  // WS-agnostic.
  import {
    createViewport, screenToImage, fit as vpFit, oneToOne as vpOneToOne, zoomAt, panBy,
  } from "../lib/viewport.mjs";

  const host = $host();
  let canvas;
  const vp = createViewport();
  let img = null;   // current HTMLImageElement
  let off = null;   // offscreen native-size canvas for pixel probe

  function render() {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    ctx.imageSmoothingEnabled = false;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (!img) return;
    ctx.setTransform(vp.scale, 0, 0, vp.scale, vp.panX, vp.panY);
    ctx.drawImage(img, 0, 0);
    ctx.setTransform(1, 0, 0, 1, 0, 0);
  }

  function resize() {
    if (!canvas) return;
    const r = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.round(r.width));
    canvas.height = Math.max(1, Math.round(r.height));
    vp.viewW = canvas.width;
    vp.viewH = canvas.height;
    render();
  }

  function emit(type, detail) {
    host.dispatchEvent(new CustomEvent(type, { detail, bubbles: true, composed: true }));
  }

  function setFrame(src) {
    const im = new Image();
    im.onload = () => {
      const first = !vp.imgW;
      img = im;
      vp.imgW = im.naturalWidth || im.width;
      vp.imgH = im.naturalHeight || im.height;
      off = document.createElement("canvas");
      off.width = vp.imgW;
      off.height = vp.imgH;
      off.getContext("2d").drawImage(im, 0, 0);
      if (first) vpFit(vp);
      render();
    };
    im.src = typeof src === "string" ? src : src.dataUrl;
  }

  function onWheel(e) {
    if (!img) return;
    e.preventDefault();
    const r = canvas.getBoundingClientRect();
    zoomAt(vp, e.clientX - r.left, e.clientY - r.top, e.deltaY < 0 ? 1.15 : 1 / 1.15);
    render();
    emit("viewchange", { scale: vp.scale });
  }

  let drag = null;
  let moved = false;
  function onDown(e) { if (img) { drag = { x: e.clientX, y: e.clientY }; moved = false; canvas.setPointerCapture?.(e.pointerId); } }
  function onMove(e) {
    if (!drag) return;
    const dx = e.clientX - drag.x, dy = e.clientY - drag.y;
    if (dx || dy) moved = true;
    panBy(vp, dx, dy);
    drag = { x: e.clientX, y: e.clientY };
    render();
  }
  function onUp(e) {
    if (drag && !moved) pick(e);   // a click, not a drag → probe
    drag = null;
  }
  function pick(e) {
    if (!img || !off) return;
    const r = canvas.getBoundingClientRect();
    const p = screenToImage(vp, e.clientX - r.left, e.clientY - r.top);
    const ix = Math.floor(p.x), iy = Math.floor(p.y);
    let rgb = null;
    if (ix >= 0 && iy >= 0 && ix < vp.imgW && iy < vp.imgH) {
      const d = off.getContext("2d").getImageData(ix, iy, 1, 1).data;
      rgb = [d[0], d[1], d[2]];
    }
    emit("pixelpick", { x: ix, y: iy, rgb });
  }

  // Attach the imperative tap-out API + observe resize once the canvas exists.
  $effect(() => {
    host.setFrame = setFrame;
    host.fit = () => { vpFit(vp); render(); emit("viewchange", { scale: vp.scale }); };
    host.oneToOne = () => { vpOneToOne(vp); render(); emit("viewchange", { scale: vp.scale }); };
    resize();
    const ro = new ResizeObserver(resize);
    ro.observe(canvas);
    return () => ro.disconnect();
  });
</script>

<canvas
  bind:this={canvas}
  onwheel={onWheel}
  onpointerdown={onDown}
  onpointermove={onMove}
  onpointerup={onUp}
></canvas>

<style>
  :host { display: block; width: 100%; height: 100%; }
  canvas {
    width: 100%; height: 100%; display: block;
    background: var(--xi-viewer-bg, #111);
    cursor: grab; touch-action: none; image-rendering: pixelated;
  }
  canvas:active { cursor: grabbing; }
</style>
