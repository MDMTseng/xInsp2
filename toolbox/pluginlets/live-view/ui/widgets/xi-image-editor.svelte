<svelte:options customElement={{ tag: "xi-image-editor", props: { tool: {}, label: {} } }} />

<script>
  // Pull-model teach editor (task #77): show a frame, let the user draw with the
  // active tool (point/rect/polygon — pointer events mapped to IMAGE coords via
  // viewport.mjs), and emit `commit` {tool, result} / `cancel`. The host sends the
  // result to the plugin via exchange_instance — no backend/ABI change. Tap-out
  // methods on the host: setFrame(src) / setTool(id) / getResult().
  import { createViewport, screenToImage, imageToScreen, fit as vpFit, zoomAt } from "../lib/viewport.mjs";
  import { makeTool } from "../lib/tools.mjs";

  let { tool = "rect", label = "" } = $props();
  const host = $host();
  let canvas;
  const vp = createViewport();
  let img = null;
  let active = makeTool(tool);

  const toScreen = (ip) => imageToScreen(vp, ip.x, ip.y);

  function render() {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.imageSmoothingEnabled = false;
    ctx.setTransform(1, 0, 0, 1, 0, 0);
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    if (img) {
      ctx.setTransform(vp.scale, 0, 0, vp.scale, vp.panX, vp.panY);
      ctx.drawImage(img, 0, 0);
      ctx.setTransform(1, 0, 0, 1, 0, 0);
    }
    if (active) active.draw(ctx, toScreen);
  }

  function resize() {
    if (!canvas) return;
    const r = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.round(r.width));
    canvas.height = Math.max(1, Math.round(r.height));
    vp.viewW = canvas.width; vp.viewH = canvas.height;
    render();
  }

  // Accept a string/dataUrl (decode here) or an already-decoded drawable shared
  // by the XiClient (skips a redundant decode when the image is shown in several
  // components). Borrowed only for drawing — never closed.
  function isDrawable(s) {
    return !!s && typeof s !== "string" && !("dataUrl" in s) && (
      (typeof HTMLImageElement !== "undefined" && s instanceof HTMLImageElement) ||
      (typeof HTMLCanvasElement !== "undefined" && s instanceof HTMLCanvasElement) ||
      (typeof OffscreenCanvas !== "undefined" && s instanceof OffscreenCanvas) ||
      (typeof ImageBitmap !== "undefined" && s instanceof ImageBitmap));
  }
  function setFrame(src) {
    if (isDrawable(src)) { useFrame(src); return; }
    const im = new Image();
    im.onload = () => useFrame(im);
    im.src = typeof src === "string" ? src : src.dataUrl;
  }
  function useFrame(im) {
    const first = !vp.imgW;
    img = im; vp.imgW = im.naturalWidth || im.width; vp.imgH = im.naturalHeight || im.height;
    if (first) vpFit(vp);
    render();
  }

  function setTool(id) { active = makeTool(id) || active; render(); }

  const ip = (e) => { const r = canvas.getBoundingClientRect(); return screenToImage(vp, e.clientX - r.left, e.clientY - r.top); };
  function onDown(e) { if (active) { active.onDown(ip(e)); render(); } }
  function onMove(e) { if (active && e.buttons) { active.onMove(ip(e)); render(); } }
  function onUp(e)   { if (active) { active.onUp(ip(e)); render(); } }
  function onDbl(e)  { if (active) { active.onDbl(ip(e)); render(); } }
  function onWheel(e) {
    if (!img) return;
    e.preventDefault();
    const r = canvas.getBoundingClientRect();
    zoomAt(vp, e.clientX - r.left, e.clientY - r.top, e.deltaY < 0 ? 1.15 : 1 / 1.15);
    render();
  }

  function commit() {
    if (!active || !active.done()) return;
    host.dispatchEvent(new CustomEvent("commit", { detail: { tool: active.type, result: active.result() }, bubbles: true, composed: true }));
  }
  function cancel() {
    host.dispatchEvent(new CustomEvent("cancel", { detail: {}, bubbles: true, composed: true }));
  }

  $effect(() => {
    host.setFrame = setFrame;
    host.setTool = setTool;
    host.getResult = () => (active && active.done() ? active.result() : null);
    resize();
    const ro = new ResizeObserver(resize);
    ro.observe(canvas);
    return () => ro.disconnect();
  });
</script>

<div class="xi-editor">
  <div class="bar">
    <span class="lbl">{label || tool}</span>
    <span class="spacer"></span>
    <button class="cancel" onclick={cancel}>Cancel</button>
    <button class="commit" onclick={commit}>Commit</button>
  </div>
  <canvas bind:this={canvas} onpointerdown={onDown} onpointermove={onMove} onpointerup={onUp} ondblclick={onDbl} onwheel={onWheel}></canvas>
</div>

<style>
  :host { display: block; width: 100%; height: 100%; }
  .xi-editor { display: flex; flex-direction: column; height: 100%; }
  .bar { display: flex; align-items: center; gap: 0.5rem; padding: 0.35rem 0.5rem; font: 13px system-ui, sans-serif; background: var(--xi-bar-bg, #1f2937); color: #e5e7eb; }
  .spacer { flex: 1; }
  button { padding: 0.2rem 0.7rem; border: 0; border-radius: 4px; cursor: pointer; font: inherit; }
  .commit { background: var(--xi-accent, #6366f1); color: #fff; }
  .cancel { background: #374151; color: #e5e7eb; }
  canvas { flex: 1; width: 100%; display: block; background: var(--xi-viewer-bg, #111); cursor: crosshair; touch-action: none; image-rendering: pixelated; }
</style>
