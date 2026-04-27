<!DOCTYPE html>
<html><head><meta charset="utf-8">
<style>
  body { font-family: var(--vscode-font-family); color: var(--vscode-foreground);
         background: var(--vscode-sideBar-background); padding: 16px;
         margin: 0; font-size: 13px; line-height: 1.5; }
  h2 { margin: 0 0 12px; font-size: 15px; font-weight: 600; }
  .hint { color: var(--vscode-descriptionForeground); font-size: 12px; margin-bottom: 16px; }
  .card { background: var(--vscode-editor-background);
          border: 1px solid var(--vscode-widget-border, rgba(255,255,255,0.08));
          border-radius: 6px; padding: 14px 16px; margin-bottom: 12px; }
  .row { display: flex; align-items: center; gap: 10px; margin-bottom: 8px; }
  label { min-width: 90px; color: var(--vscode-descriptionForeground); font-size: 12px; }
  input[type=range] { flex: 1; }
  .val { min-width: 38px; text-align: right; font-family: monospace; font-size: 12px; }
  .stat { display: flex; gap: 12px; padding: 6px 0; font-family: monospace; }
  .stat .k { color: var(--vscode-descriptionForeground); }
  .stat .v { font-weight: 600; }
</style></head>
<body>
  <h2>{{NAME}} — image processor</h2>
  <div class="hint">Threshold + foreground percentage. Drag the slider to update the plugin live.</div>
  <div class="card">
    <div class="row">
      <label>Threshold</label>
      <input id="thr" type="range" min="0" max="255" value="128">
      <span id="thrV" class="val">128</span>
    </div>
  </div>
  <div class="card">
    <div class="stat"><span class="k">Last fg %:</span>     <span id="fg" class="v">—</span></div>
    <div class="stat"><span class="k">Active threshold:</span> <span id="active" class="v">—</span></div>
  </div>
  <div class="card">
    <div style="font-size:11px; opacity:0.6; margin-bottom:6px;">
      Output preview (drag to pan, wheel to zoom around cursor)
    </div>
    {{INCLUDE _shared/image_viewer_widget.html}}
  </div>
<script>
  const vscode = acquireVsCodeApi();
  const thr = document.getElementById('thr');
  const thrV = document.getElementById('thrV');
  const fg = document.getElementById('fg');
  const active = document.getElementById('active');
  let pending = false;
  function send(cmdObj) {
      vscode.postMessage({ type: 'exchange', cmd: JSON.stringify(cmdObj) });
  }
  function requestPreview() { vscode.postMessage({ type: 'request_preview' }); }
  thr.addEventListener('input', () => {
      thrV.textContent = thr.value;
      if (pending) return;
      pending = true;
      send({ command: 'set_threshold', value: parseInt(thr.value) });
      setTimeout(() => { pending = false; }, 80);
  });
  window.addEventListener('message', e => {
      const m = e.data;
      if (!m) return;
      if (m.type === 'status') {
          if (typeof m.threshold === 'number') {
              active.textContent = m.threshold;
              if (!thr.matches(':active')) {
                  thr.value = m.threshold;
                  thrV.textContent = m.threshold;
              }
          }
          if (typeof m.last_fg_pct === 'number') {
              fg.textContent = (m.last_fg_pct * 100).toFixed(2) + '%';
              requestPreview();
          }
      } else if (m.type === 'preview' && m.jpeg) {
          // Inline pan + cursor-anchored zoom — no separate tab.
          window.xiViewer && window.xiViewer.preview &&
              window.xiViewer.preview.setImage(m.jpeg, m.width, m.height);
      }
  });
  send({ command: 'get_status' });
  requestPreview();
</script>
</body></html>
