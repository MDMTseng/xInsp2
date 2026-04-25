// imageViewerWidget.ts — embeddable pan + cursor-anchored zoom widget
// for plugin UIs (and anywhere else that wants inline image preview
// with the same interaction model as the standalone viewer).
//
// Render returns one self-contained string with <style>, <div>, and
// <script>. Drop it into any webview HTML; the script auto-attaches
// to its container.
//
// API exposed on window after render:
//
//   window.xiViewer[<id>].setImage(jpegBase64, width?, height?)
//   window.xiViewer[<id>].fit()
//   window.xiViewer[<id>].oneToOne()
//
// Plugin UIs can just call setImage() whenever they receive a preview
// message; the widget handles all interaction (drag-pan + wheel-zoom
// at cursor) on its own.

export function imageViewerWidget(id: string, opts?: { height?: string }): string {
    const h = (opts && opts.height) || '320px';
    return `
<style>
  .xiv-${id} { position: relative; width: 100%; height: ${h};
               overflow: hidden; touch-action: none;
               background: repeating-conic-gradient(#1d1d1d 0% 25%, #2a2a2a 0% 50%) 50% / 16px 16px;
               border: 1px solid var(--vscode-widget-border, rgba(255,255,255,0.08));
               border-radius: 4px; }
  .xiv-${id} canvas { position: absolute; top: 0; left: 0; cursor: grab;
                      image-rendering: pixelated; }
  .xiv-${id} canvas.dragging { cursor: grabbing; }
  .xiv-${id}-empty { position: absolute; inset: 0;
                     display: flex; align-items: center; justify-content: center;
                     opacity: 0.5; pointer-events: none; font-size: 12px; }
  .xiv-${id}-status { position: absolute; right: 6px; bottom: 6px;
                      padding: 2px 6px; border-radius: 3px;
                      background: rgba(0,0,0,0.55); color: #ddd;
                      font: 11px var(--vscode-editor-font-family, monospace);
                      pointer-events: none; }
</style>
<div class="xiv-${id}" id="xiv-${id}">
  <canvas id="xiv-${id}-cv" width="0" height="0"></canvas>
  <div class="xiv-${id}-empty" id="xiv-${id}-empty">no image yet</div>
  <div class="xiv-${id}-status" id="xiv-${id}-status">—</div>
</div>
<script>
(function() {
  const stage  = document.getElementById('xiv-${id}');
  const canvas = document.getElementById('xiv-${id}-cv');
  const ctx    = canvas.getContext('2d');
  const empty  = document.getElementById('xiv-${id}-empty');
  const status = document.getElementById('xiv-${id}-status');

  let imgW = 0, imgH = 0;
  let pan = { x: 0, y: 0 };
  let scale = 1;
  let bitmap = null;

  function applyTransform() {
      canvas.style.left = pan.x + 'px';
      canvas.style.top  = pan.y + 'px';
      canvas.width  = Math.max(1, Math.round(imgW * scale));
      canvas.height = Math.max(1, Math.round(imgH * scale));
      if (bitmap) {
          ctx.imageSmoothingEnabled = false;
          ctx.drawImage(bitmap, 0, 0, canvas.width, canvas.height);
      }
      updateStatus();
  }
  function updateStatus(extra) {
      const z = (scale * 100).toFixed(0);
      status.textContent = imgW
          ? imgW + 'x' + imgH + '  ' + z + '%' + (extra ? '  ' + extra : '')
          : '—';
  }
  function fit() {
      if (!imgW) return;
      const s = stage.clientWidth, h = stage.clientHeight;
      scale = Math.min(s / imgW, h / imgH) * 0.95;
      pan.x = (s - imgW * scale) / 2;
      pan.y = (h - imgH * scale) / 2;
      applyTransform();
  }
  function oneToOne() {
      if (!imgW) return;
      scale = 1;
      pan.x = (stage.clientWidth - imgW) / 2;
      pan.y = (stage.clientHeight - imgH) / 2;
      applyTransform();
  }
  function zoomAt(sx, sy, factor) {
      const ix = (sx - pan.x) / scale;
      const iy = (sy - pan.y) / scale;
      let next = scale * factor;
      next = Math.max(0.05, Math.min(64, next));
      scale = next;
      pan.x = sx - ix * scale;
      pan.y = sy - iy * scale;
      applyTransform();
  }

  stage.addEventListener('wheel', (e) => {
      if (!imgW) return;
      e.preventDefault();
      const r = stage.getBoundingClientRect();
      zoomAt(e.clientX - r.left, e.clientY - r.top, e.deltaY < 0 ? 1.15 : 1/1.15);
  }, { passive: false });

  let dragStart = null;
  stage.addEventListener('mousedown', (e) => {
      if (!imgW) return;
      const r = stage.getBoundingClientRect();
      dragStart = { sx: e.clientX - r.left, sy: e.clientY - r.top, pan0: { ...pan } };
      canvas.classList.add('dragging');
  });
  stage.addEventListener('mousemove', (e) => {
      const r = stage.getBoundingClientRect();
      const sx = e.clientX - r.left, sy = e.clientY - r.top;
      if (imgW) {
          const ix = (sx - pan.x) / scale, iy = (sy - pan.y) / scale;
          updateStatus('x=' + Math.round(ix) + ' y=' + Math.round(iy));
      }
      if (!dragStart) return;
      pan.x = dragStart.pan0.x + (sx - dragStart.sx);
      pan.y = dragStart.pan0.y + (sy - dragStart.sy);
      applyTransform();
  });
  stage.addEventListener('mouseup',    () => { dragStart = null; canvas.classList.remove('dragging'); });
  stage.addEventListener('mouseleave', () => { dragStart = null; canvas.classList.remove('dragging'); });

  async function setImage(jpegBase64, w, h) {
      empty.style.display = 'none';
      const blob = await (await fetch('data:image/jpeg;base64,' + jpegBase64)).blob();
      bitmap = await createImageBitmap(blob);
      imgW = w || bitmap.width;
      imgH = h || bitmap.height;
      fit();
  }

  if (!window.xiViewer) window.xiViewer = {};
  window.xiViewer['${id}'] = { setImage, fit, oneToOne };
})();
</script>
`;
}
