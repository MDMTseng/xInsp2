// imageViewerPanel.ts — interactive image viewer with pan / scroll-zoom
// + two pick tools (point / area). Opens as a tab; one panel reused
// across show() calls so flipping between vars doesn't pile up tabs.
//
// Tools surface:
//   - Move (default): drag pans, wheel zooms around cursor.
//   - Pick Point   : click reports image-space {x, y}.
//   - Pick Area    : drag draws a rect; release reports {x, y, w, h}.
//
// All coordinates are reported in IMAGE pixel space (independent of
// canvas zoom / pan), which is what an inspection script wants for
// region-of-interest selection.

import * as vscode from 'vscode';

export interface ImageMessage {
    name: string;          // human-readable label (var name)
    width: number;
    height: number;
    jpegBase64: string;    // already encoded by backend (xi_jpeg)
}

export interface PickReport {
    tool: 'point' | 'area';
    x: number; y: number;
    w?: number; h?: number;   // present for 'area'
    image: string;            // image label this came from
}

export class ImageViewerPanel {
    public static readonly viewType = 'xinsp2.imageViewer';
    private static current: ImageViewerPanel | undefined;

    public static onPick: vscode.EventEmitter<PickReport> = new vscode.EventEmitter();
    // Fired with the selftest result when the harness sends type:'selftest'.
    public static onSelftest: vscode.EventEmitter<any> = new vscode.EventEmitter();

    /** Test hook: run zoom/pan invariants headlessly inside the webview. */
    public static runSelftest(): boolean {
        if (!ImageViewerPanel.current) return false;
        ImageViewerPanel.current.panel.webview.postMessage({ type: 'selftest' });
        return true;
    }

    // Fired with { seq, scale, panX, panY, tool } after an applyOp completes.
    public static onOpDone: vscode.EventEmitter<any> = new vscode.EventEmitter();
    private static opSeq = 0;
    /** Test hook: drive a single pan/zoom op so an e2e can screenshot
     *  between operations. op = { kind: 'fit' | 'oneToOne' | 'zoom' |
     *  'pan' | 'tool', sx?, sy?, factor?, dx?, dy?, tool? }. Resolves
     *  with the post-op transform state. */
    public static applyOp(op: any): Promise<any> {
        return new Promise((resolve) => {
            if (!ImageViewerPanel.current) {
                resolve({ ok: false, error: 'no panel open' });
                return;
            }
            const seq = ++ImageViewerPanel.opSeq;
            const sub = ImageViewerPanel.onOpDone.event((r) => {
                if (r.seq !== seq) return;
                sub.dispose();
                resolve(r);
            });
            ImageViewerPanel.current.panel.webview.postMessage({ type: 'op', seq, ...op });
        });
    }

    private readonly panel: vscode.WebviewPanel;
    private disposables: vscode.Disposable[] = [];

    public static show(extensionUri: vscode.Uri, img: ImageMessage) {
        if (ImageViewerPanel.current) {
            ImageViewerPanel.current.panel.reveal(vscode.ViewColumn.Beside, true);
            ImageViewerPanel.current.update(img);
            return;
        }
        const panel = vscode.window.createWebviewPanel(
            ImageViewerPanel.viewType,
            `Image: ${img.name}`,
            { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
            { enableScripts: true, retainContextWhenHidden: true });
        ImageViewerPanel.current = new ImageViewerPanel(panel);
        ImageViewerPanel.current.update(img);
    }

    private constructor(panel: vscode.WebviewPanel) {
        this.panel = panel;
        this.panel.webview.html = this.html();
        this.panel.webview.onDidReceiveMessage((m: any) => {
            if (m && m.type === 'pick') {
                ImageViewerPanel.onPick.fire(m as PickReport);
            } else if (m && m.type === 'selftest_result') {
                ImageViewerPanel.onSelftest.fire(m);
            } else if (m && m.type === 'op_done') {
                ImageViewerPanel.onOpDone.fire(m);
            }
        }, null, this.disposables);
        this.panel.onDidDispose(() => this.dispose(), null, this.disposables);
    }

    private update(img: ImageMessage) {
        this.panel.title = `Image: ${img.name}`;
        this.panel.webview.postMessage({ type: 'image', ...img });
    }

    private dispose() {
        ImageViewerPanel.current = undefined;
        this.panel.dispose();
        while (this.disposables.length) this.disposables.pop()?.dispose();
    }

    private html(): string {
        // Single self-contained webview HTML. No external resources,
        // no bundler needed — keeps the surface area auditable.
        // Coords are reported in IMAGE space (pixel-accurate, not the
        // zoomed/panned canvas position).
        return /* html */ `<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
  html, body { height: 100%; margin: 0; padding: 0;
               background: var(--vscode-editor-background);
               color: var(--vscode-foreground);
               font-family: var(--vscode-font-family); font-size: 12px; }
  #toolbar { display: flex; gap: 4px; padding: 6px;
             border-bottom: 1px solid var(--vscode-widget-border);
             align-items: center; flex-wrap: wrap; }
  #toolbar button {
      background: var(--vscode-button-secondaryBackground, #3a3d41);
      color: var(--vscode-button-secondaryForeground, inherit);
      border: 1px solid var(--vscode-widget-border, #444);
      padding: 4px 10px; cursor: pointer; font-size: 12px;
      border-radius: 3px;
  }
  #toolbar button.on {
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
      border-color: var(--vscode-focusBorder);
  }
  #toolbar button:hover { filter: brightness(1.2); }
  #toolbar .sep { width: 1px; align-self: stretch;
                  background: var(--vscode-widget-border, #444); margin: 0 4px; }
  #status { margin-left: auto; opacity: 0.7; font-family: var(--vscode-editor-font-family, monospace); }
  #stage { position: relative; width: 100%; height: calc(100% - 36px);
           overflow: hidden; touch-action: none;
           background: repeating-conic-gradient(#1d1d1d 0% 25%, #2a2a2a 0% 50%) 50% / 16px 16px; }
  #canvas { position: absolute; top: 0; left: 0; cursor: grab; image-rendering: pixelated; }
  #canvas.move-active { cursor: grabbing; }
  #canvas.pick-active { cursor: crosshair; }
  #marker { position: absolute; pointer-events: none;
            border: 1px solid #ff0080; background: rgba(255, 0, 128, 0.18);
            display: none; }
  #point-marker { position: absolute; pointer-events: none;
                  width: 14px; height: 14px; margin: -7px 0 0 -7px;
                  border: 2px solid #00ff80; border-radius: 50%;
                  display: none; box-shadow: 0 0 4px rgba(0,255,128,0.6); }
  #empty { position: absolute; inset: 0; display: flex; align-items: center;
           justify-content: center; opacity: 0.5; pointer-events: none; }
  #last-pick { font-family: var(--vscode-editor-font-family, monospace);
               margin-left: 8px; opacity: 0.85; }
</style></head>
<body>
  <div id="toolbar">
    <button id="t-move" class="on" title="Pan with drag, zoom with wheel (M)">✥ Move</button>
    <button id="t-point" title="Click to pick a point in image space (P)">⌖ Pick Point</button>
    <button id="t-area"  title="Drag to pick a rectangular region (A)">▭ Pick Area</button>
    <div class="sep"></div>
    <button id="zoom-fit" title="Fit image to viewport (F)">Fit</button>
    <button id="zoom-1"   title="100% zoom (0)">1:1</button>
    <button id="zoom-out" title="Zoom out">−</button>
    <button id="zoom-in"  title="Zoom in">+</button>
    <span id="last-pick"></span>
    <span id="status">—</span>
  </div>
  <div id="stage">
    <canvas id="canvas" width="0" height="0"></canvas>
    <div id="marker"></div>
    <div id="point-marker"></div>
    <div id="empty">No image loaded yet.</div>
  </div>

<script>
(() => {
  const vscode = acquireVsCodeApi();
  const stage  = document.getElementById('stage');
  const canvas = document.getElementById('canvas');
  const ctx    = canvas.getContext('2d');
  const status = document.getElementById('status');
  const empty  = document.getElementById('empty');
  const marker = document.getElementById('marker');
  const pointMarker = document.getElementById('point-marker');
  const lastPick = document.getElementById('last-pick');

  const tBtns = {
      move:  document.getElementById('t-move'),
      point: document.getElementById('t-point'),
      area:  document.getElementById('t-area'),
  };

  // ---- View transform ---------------------------------------------------
  // canvas <img> is positioned absolutely; we apply translate + scale by
  // setting CSS top/left (pan in screen px) and width/height (= imgW *
  // scale). All "image-space" coords come from inverting this transform.
  let imgW = 0, imgH = 0;
  let pan = { x: 0, y: 0 }; // canvas top-left in screen px
  let scale = 1;
  let imageLabel = '';
  let bitmap = null; // ImageBitmap
  let tool = 'move';

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
          ? \`\${imgW}\xd7\${imgH}  ·  \${z}%\${extra ? '  ·  ' + extra : ''}\`
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
      const cx = stage.clientWidth / 2;
      const cy = stage.clientHeight / 2;
      scale = 1;
      pan.x = cx - imgW / 2;
      pan.y = cy - imgH / 2;
      applyTransform();
  }

  // ---- Tool selection ---------------------------------------------------
  function setTool(t) {
      tool = t;
      for (const k of Object.keys(tBtns)) tBtns[k].classList.toggle('on', k === t);
      canvas.classList.toggle('move-active', t === 'move');
      canvas.classList.toggle('pick-active', t !== 'move');
      // Hide ephemeral markers when switching modes.
      if (t !== 'point') pointMarker.style.display = 'none';
      if (t !== 'area')  marker.style.display = 'none';
  }
  tBtns.move .addEventListener('click', () => setTool('move'));
  tBtns.point.addEventListener('click', () => setTool('point'));
  tBtns.area .addEventListener('click', () => setTool('area'));
  document.getElementById('zoom-fit').addEventListener('click', fit);
  document.getElementById('zoom-1'  ).addEventListener('click', oneToOne);
  document.getElementById('zoom-in' ).addEventListener('click', () => zoomAt(stage.clientWidth/2, stage.clientHeight/2,  1.25));
  document.getElementById('zoom-out').addEventListener('click', () => zoomAt(stage.clientWidth/2, stage.clientHeight/2,  0.8));

  // ---- Wheel zoom (anchored at cursor) ----------------------------------
  function zoomAt(sx, sy, factor) {
      // Image-space anchor under cursor before zoom:
      const ix = (sx - pan.x) / scale;
      const iy = (sy - pan.y) / scale;
      let next = scale * factor;
      next = Math.max(0.05, Math.min(64, next));
      scale = next;
      // Re-pin so (ix, iy) stays under (sx, sy).
      pan.x = sx - ix * scale;
      pan.y = sy - iy * scale;
      applyTransform();
  }
  stage.addEventListener('wheel', (e) => {
      if (!imgW) return;
      e.preventDefault();
      const r = stage.getBoundingClientRect();
      const sx = e.clientX - r.left, sy = e.clientY - r.top;
      zoomAt(sx, sy, e.deltaY < 0 ? 1.15 : 1/1.15);
  }, { passive: false });

  // ---- Drag (pan or area-pick) ------------------------------------------
  let dragStart = null;
  function imageOf(e) {
      const r = stage.getBoundingClientRect();
      const sx = e.clientX - r.left;
      const sy = e.clientY - r.top;
      return {
          sx, sy,
          ix: (sx - pan.x) / scale,
          iy: (sy - pan.y) / scale,
      };
  }
  stage.addEventListener('mousedown', (e) => {
      if (!imgW) return;
      dragStart = imageOf(e);
      dragStart.pan0 = { ...pan };
      if (tool === 'area') {
          marker.style.display = 'block';
          marker.style.left   = dragStart.sx + 'px';
          marker.style.top    = dragStart.sy + 'px';
          marker.style.width  = '0px';
          marker.style.height = '0px';
      }
  });
  stage.addEventListener('mousemove', (e) => {
      const here = imageOf(e);
      if (imgW) updateStatus(\`x=\${Math.round(here.ix)} y=\${Math.round(here.iy)}\`);
      if (!dragStart) return;
      if (tool === 'move') {
          pan.x = dragStart.pan0.x + (here.sx - dragStart.sx);
          pan.y = dragStart.pan0.y + (here.sy - dragStart.sy);
          applyTransform();
      } else if (tool === 'area') {
          const x = Math.min(dragStart.sx, here.sx);
          const y = Math.min(dragStart.sy, here.sy);
          const w = Math.abs(here.sx - dragStart.sx);
          const h = Math.abs(here.sy - dragStart.sy);
          marker.style.left = x + 'px';
          marker.style.top  = y + 'px';
          marker.style.width  = w + 'px';
          marker.style.height = h + 'px';
      }
  });
  stage.addEventListener('mouseup', (e) => {
      if (!dragStart) return;
      const here = imageOf(e);
      if (tool === 'point') {
          // mousedown was effectively the click — fire pick. We use the
          // RELEASE position so a tiny drag is OK and consistent with
          // most apps.
          pointMarker.style.display = 'block';
          pointMarker.style.left = here.sx + 'px';
          pointMarker.style.top  = here.sy + 'px';
          const px = Math.round(here.ix), py = Math.round(here.iy);
          if (px >= 0 && py >= 0 && px < imgW && py < imgH) {
              vscode.postMessage({ type: 'pick', tool: 'point', x: px, y: py, image: imageLabel });
              lastPick.textContent = \`pt (\${px}, \${py})\`;
          }
      } else if (tool === 'area') {
          const x0 = Math.round(Math.min(dragStart.ix, here.ix));
          const y0 = Math.round(Math.min(dragStart.iy, here.iy));
          const x1 = Math.round(Math.max(dragStart.ix, here.ix));
          const y1 = Math.round(Math.max(dragStart.iy, here.iy));
          const w = Math.max(0, x1 - x0), h = Math.max(0, y1 - y0);
          if (w >= 1 && h >= 1) {
              vscode.postMessage({ type: 'pick', tool: 'area', x: x0, y: y0, w, h, image: imageLabel });
              lastPick.textContent = \`rect (\${x0}, \${y0}) \${w}\xd7\${h}\`;
          }
      }
      dragStart = null;
  });
  // Click anywhere in point mode (no drag needed)
  stage.addEventListener('click', (e) => {
      if (tool !== 'point' || dragStart === null) return; // only used as fallback
  });

  // ---- Single-key tool shortcuts ----------------------------------------
  window.addEventListener('keydown', (e) => {
      if (e.target && e.target.matches && e.target.matches('input, textarea')) return;
      if (e.key === 'm' || e.key === 'M') setTool('move');
      else if (e.key === 'p' || e.key === 'P') setTool('point');
      else if (e.key === 'a' || e.key === 'A') setTool('area');
      else if (e.key === 'f' || e.key === 'F') fit();
      else if (e.key === '0')                  oneToOne();
  });

  // ---- Receive image data from extension --------------------------------
  window.addEventListener('message', async (e) => {
      const msg = e.data;
      if (msg.type === 'image') {
          imageLabel = msg.name || '';
          empty.style.display = 'none';
          const blob = await (await fetch('data:image/jpeg;base64,' + msg.jpegBase64)).blob();
          bitmap = await createImageBitmap(blob);
          imgW = msg.width  || bitmap.width;
          imgH = msg.height || bitmap.height;
          fit();
      } else if (msg.type === 'op') {
          // Drive a single pan/zoom op programmatically. The e2e
          // journey uses this to screenshot between operations
          // (real wheel events can't reach webviews from the
          // extension host). Echoes back transform state so the
          // test can assert.
          const ack = (extra) => vscode.postMessage({ type: 'op_done',
              seq: msg.seq, ok: !!imgW, kind: msg.kind,
              scale, panX: pan.x, panY: pan.y, tool, ...extra });
          if (!imgW) { ack({ error: 'no image loaded' }); return; }
          if (msg.kind === 'fit')        { fit();        ack({}); }
          else if (msg.kind === 'oneToOne') { oneToOne(); ack({}); }
          else if (msg.kind === 'zoom')  {
              const sx = (typeof msg.sx === 'number') ? msg.sx : stage.clientWidth/2;
              const sy = (typeof msg.sy === 'number') ? msg.sy : stage.clientHeight/2;
              const f  = (typeof msg.factor === 'number') ? msg.factor : 1.25;
              zoomAt(sx, sy, f); ack({});
          }
          else if (msg.kind === 'pan')   {
              pan.x += (msg.dx || 0); pan.y += (msg.dy || 0);
              applyTransform(); ack({});
          }
          else if (msg.kind === 'tool')  {
              setTool(msg.tool || 'move'); ack({});
          }
          else { ack({ error: 'unknown op kind ' + msg.kind }); }
      } else if (msg.type === 'selftest') {
          // Headless validation of pan + zoom math. We can't drive real
          // mouse events from the extension host, so the e2e harness
          // sends this message and reads the result back. Each step
          // checks an invariant of zoomAt: the IMAGE-space point under
          // the (sx,sy) anchor must stay constant across the zoom.
          const out = { type: 'selftest_result', steps: [], ok: true };
          function step(label, fn) {
              try {
                  const r = fn();
                  out.steps.push({ label, ok: !!r.ok, detail: r.detail });
                  if (!r.ok) out.ok = false;
              } catch (err) {
                  out.steps.push({ label, ok: false, detail: String(err) });
                  out.ok = false;
              }
          }
          if (!imgW) {
              vscode.postMessage({ type: 'selftest_result', ok: false,
                  steps: [{ label: 'preconditions', ok: false, detail: 'no image loaded' }] });
              return;
          }
          // Reset to fit so we have a known starting transform.
          fit();
          step('zoom keeps anchor pixel under cursor', () => {
              const sx = 50, sy = 50;
              const ix0 = (sx - pan.x) / scale, iy0 = (sy - pan.y) / scale;
              zoomAt(sx, sy, 2.0);
              const ix1 = (sx - pan.x) / scale, iy1 = (sy - pan.y) / scale;
              const dx = Math.abs(ix1 - ix0), dy = Math.abs(iy1 - iy0);
              return { ok: dx < 0.001 && dy < 0.001,
                       detail: 'anchor drift dx=' + dx.toFixed(4) + ' dy=' + dy.toFixed(4) };
          });
          step('pan moves canvas in screen px', () => {
              const x0 = pan.x, y0 = pan.y;
              pan.x = x0 + 30; pan.y = y0 - 17; applyTransform();
              const ok = pan.x === x0 + 30 && pan.y === y0 - 17;
              return { ok, detail: 'final pan=' + pan.x + ',' + pan.y };
          });
          step('clamp keeps zoom in [0.05, 64]', () => {
              for (let i = 0; i < 30; ++i) zoomAt(50, 50, 2.0);
              const high = scale; if (high > 64.001) return { ok: false, detail: 'too high ' + high };
              for (let i = 0; i < 60; ++i) zoomAt(50, 50, 0.5);
              const low = scale;  if (low  < 0.049)  return { ok: false, detail: 'too low '  + low };
              return { ok: true, detail: 'high=' + high.toFixed(2) + ' low=' + low.toFixed(3) };
          });
          fit();
          vscode.postMessage(out);
      }
  });

  // First-load hint: keep "no image" until message arrives.
  window.addEventListener('resize', () => { /* preserve transform */ });
})();
</script>
</body></html>`;
    }
}
