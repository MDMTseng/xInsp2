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
  input[type=number] { width: 90px; padding: 4px 6px;
                       background: var(--vscode-input-background);
                       color: var(--vscode-input-foreground);
                       border: 1px solid var(--vscode-input-border, #555); border-radius: 3px; }
  .stat { display: flex; gap: 12px; padding: 6px 0; font-family: monospace; }
  .stat .k { color: var(--vscode-descriptionForeground); }
  .stat .v { font-weight: 600; }
  button { background: var(--vscode-button-background); color: var(--vscode-button-foreground);
           border: none; padding: 6px 14px; border-radius: 3px; cursor: pointer; margin-right: 6px; }
  button:hover { background: var(--vscode-button-hoverBackground); }
  button.stop { background: #b04848; }
  button.stop:hover { background: #c25555; }
  .running { color: #4caf50; }
  .stopped { color: var(--vscode-descriptionForeground); }
</style></head>
<body>
  <h2>{{NAME}} — synthetic source</h2>
  <div class="hint">Background worker emits a frame every interval. Start to begin pushing into the trigger bus.</div>
  <div class="card">
    <div class="row">
      <label>Width</label>
      <input id="w" type="number" min="1" max="8192" value="640">
      <label style="min-width:60px">Height</label>
      <input id="h" type="number" min="1" max="8192" value="480">
    </div>
    <div class="row">
      <label>Interval ms</label>
      <input id="iv" type="number" min="1" max="10000" value="100">
      <button id="apply">Apply size/interval</button>
    </div>
  </div>
  <div class="card">
    <div class="row">
      <button id="start">Start</button>
      <button id="stop" class="stop">Stop</button>
    </div>
    <div class="stat"><span class="k">State:</span>  <span id="state" class="v stopped">stopped</span></div>
    <div class="stat"><span class="k">Frames emitted:</span> <span id="count" class="v">0</span></div>
  </div>
  <div class="card">
    <div style="font-size:11px; opacity:0.6; margin-bottom:6px;">
      Latest emitted frame (drag to pan, wheel to zoom around cursor)
    </div>
    {{INCLUDE _shared/image_viewer_widget.html}}
  </div>
<script>
  const vscode = acquireVsCodeApi();
  const w = document.getElementById('w');
  const h = document.getElementById('h');
  const iv = document.getElementById('iv');
  const stateEl = document.getElementById('state');
  const countEl = document.getElementById('count');
  function send(cmdObj) {
      vscode.postMessage({ type: 'exchange', cmd: JSON.stringify(cmdObj) });
  }
  function requestPreview() { vscode.postMessage({ type: 'request_preview' }); }
  document.getElementById('start').addEventListener('click', () => send({ command: 'start' }));
  document.getElementById('stop').addEventListener('click',  () => send({ command: 'stop'  }));
  document.getElementById('apply').addEventListener('click', () => {
      send({ command: 'set_size', width: parseInt(w.value), height: parseInt(h.value) });
      send({ command: 'set_interval', value: parseInt(iv.value) });
  });
  window.addEventListener('message', e => {
      const m = e.data;
      if (!m) return;
      if (m.type === 'preview' && m.jpeg) {
          window.xiViewer && window.xiViewer.preview &&
              window.xiViewer.preview.setImage(m.jpeg, m.width, m.height);
          return;
      }
      if (m.type !== 'status') return;
      if (typeof m.running === 'boolean') {
          stateEl.textContent = m.running ? 'running' : 'stopped';
          stateEl.classList.toggle('running', m.running);
          stateEl.classList.toggle('stopped', !m.running);
      }
      if (typeof m.count === 'number') countEl.textContent = m.count;
      if (typeof m.width  === 'number' && document.activeElement !== w)  w.value  = m.width;
      if (typeof m.height === 'number' && document.activeElement !== h)  h.value  = m.height;
      if (typeof m.interval_ms === 'number' && document.activeElement !== iv) iv.value = m.interval_ms;
  });
  // Status + preview cadence: ~1Hz. Preview only fires when running.
  setInterval(() => {
      send({ command: 'get_status' });
      if (stateEl.textContent === 'running') requestPreview();
  }, 1000);
  send({ command: 'get_status' });
</script>
</body></html>
