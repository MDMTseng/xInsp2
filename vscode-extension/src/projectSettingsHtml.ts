// projectSettingsHtml.ts — pure HTML builder for the Project Settings webview.
// No `vscode` import on purpose, so it can be rendered standalone (tests /
// screenshots) as well as inside the extension. See extension.ts for the panel
// wiring (message round-trips for toolchain + save).

export interface SettingsState {
    name: string;
    script: string;
    folder: string;
    auto_respawn: boolean;
    watchdog_ms: number;
    sources: string[];
    // Dispatch groups (parallelism.groups). Empty groups = legacy single pool.
    parallelism: {
        default_group: string;
        groups: Array<{
            name: string; max_parallel: number; thread_priority: string;
            queue_depth: number; overflow: string; result_order: string;
            cpu_affinity: any; min_interval_ms: number;
        }>;
    };
    instance_groups: Record<string, string>;   // instance name -> group ("" = default)
    // project.json "runtime" — operational knobs that apply live (set via backend
    // commands on change) and persist on Save.
    runtime: { process_priority: string; timer_fps: number };
}

export function renderProjectSettingsHtml(s: SettingsState): string {
    const esc = (x: any) => String(x ?? '').replace(/[&<>"']/g, c =>
        ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
    const checked = (b: boolean) => b ? 'checked' : '';
    // Seed data for the dynamic Parallelism editor (managed in JS).
    const parJson = JSON.stringify(s.parallelism || { default_group: '', groups: [] });
    const sourcesJson = JSON.stringify(s.sources || []);
    const instGroupsJson = JSON.stringify(s.instance_groups || {});
    const rt = s.runtime || { process_priority: '', timer_fps: -1 };
    const rtJson = JSON.stringify(rt);
    const prioOpt = (v: string) =>
        ['', 'below', 'normal', 'above', 'high', 'realtime'].map(o =>
            `<option value="${o}" ${o === (rt.process_priority || '') ? 'selected' : ''}>${o || '(unchanged)'}${o === 'realtime' ? ' ⚠' : ''}</option>`).join('');
    return `<!doctype html><html><head><meta charset="utf-8">
<style>
    body { font-family: var(--vscode-font-family); color: var(--vscode-foreground); padding: 16px 20px; max-width: 760px; }
    h1 { font-size: 1.2em; margin: 0 0 4px; }
    .folder { color: var(--vscode-descriptionForeground); font-size: 0.9em; margin-bottom: 18px; word-break: break-all; }
    section { border: 1px solid var(--vscode-panel-border); border-radius: 4px; padding: 12px 14px; margin-bottom: 14px; }
    section h2 { font-size: 0.95em; margin: 0 0 10px; text-transform: uppercase; letter-spacing: 0.04em; color: var(--vscode-descriptionForeground); }
    label.check { display: flex; align-items: center; gap: 6px; padding: 3px 0; cursor: pointer; }
    label.field { display: grid; grid-template-columns: 160px 1fr; align-items: center; gap: 10px; padding: 4px 0; }
    label.field input, label.field select { padding: 4px 6px; background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, transparent); border-radius: 2px; min-width: 0; }
    .hint { color: var(--vscode-descriptionForeground); font-size: 0.85em; margin-top: 4px; }
    .checks { display: flex; flex-wrap: wrap; gap: 6px 14px; }
    .row-buttons { display: flex; gap: 8px; margin-top: 6px; }
    button { padding: 6px 14px; background: var(--vscode-button-background); color: var(--vscode-button-foreground); border: none; border-radius: 2px; cursor: pointer; font: inherit; }
    button:hover { background: var(--vscode-button-hoverBackground); }
    button.secondary { background: var(--vscode-button-secondaryBackground); color: var(--vscode-button-secondaryForeground); }
    .saved { color: var(--vscode-charts-green); margin-left: 8px; opacity: 0; transition: opacity 0.3s; }
    .saved.show { opacity: 1; }
    .tc-row { display: grid; grid-template-columns: 18px 150px 1fr auto; align-items: center; gap: 8px; padding: 5px 0; border-top: 1px solid var(--vscode-panel-border); }
    .tc-row:first-child { border-top: none; }
    .tc-badge { font-size: 1.1em; line-height: 1; }
    .tc-ok   { color: var(--vscode-charts-green); }
    .tc-warn { color: var(--vscode-charts-red); }
    .tc-na   { color: var(--vscode-descriptionForeground); }
    .tc-label { font-weight: 600; }
    .tc-label em { font-weight: 400; color: var(--vscode-descriptionForeground); font-size: 0.85em; }
    .tc-path { word-break: break-all; font-size: 0.88em; color: var(--vscode-descriptionForeground); }
    .tc-path .tc-src { display: inline-block; margin-left: 6px; padding: 0 5px; border-radius: 8px; font-size: 0.78em; background: var(--vscode-badge-background); color: var(--vscode-badge-foreground); }
    .tc-hint { grid-column: 2 / 4; font-size: 0.82em; color: var(--vscode-charts-red); margin-top: -2px; }
    .tc-hint.muted { color: var(--vscode-descriptionForeground); }
    .tc-actions { display: flex; gap: 6px; }
    .tc-actions button { padding: 3px 9px; font-size: 0.85em; }
    /* Parallelism editor */
    .grp { border: 1px solid var(--vscode-panel-border); border-radius: 4px; padding: 8px 10px; margin-bottom: 8px; background: var(--vscode-editorWidget-background, transparent); }
    .grp-head { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
    .grp-head input.gname { font-weight: 600; flex: 0 0 150px; }
    .grp-head .spacer { flex: 1; }
    .grp-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 6px 14px; }
    .grp-grid label { display: grid; grid-template-columns: 110px 1fr; align-items: center; gap: 8px; font-size: 0.9em; }
    .grp-grid input, .grp-grid select { padding: 3px 6px; background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, transparent); border-radius: 2px; min-width: 0; }
    .grp-x { padding: 2px 9px; font-size: 0.85em; }
    .inst-grid { display: grid; grid-template-columns: 1fr 160px; gap: 4px 12px; align-items: center; margin-top: 6px; }
    .inst-grid .iname { font-size: 0.9em; }
    .inst-grid select { padding: 3px 6px; background: var(--vscode-input-background); color: var(--vscode-input-foreground); border: 1px solid var(--vscode-input-border, transparent); border-radius: 2px; }
    .subhead { font-size: 0.82em; color: var(--vscode-descriptionForeground); margin: 12px 0 2px; text-transform: uppercase; letter-spacing: 0.04em; }
</style></head>
<body>
<h1>${esc(s.name)} <span class="folder">— ${esc(s.folder)}</span></h1>

<section>
    <h2>Project</h2>
    <label class="field"><span>Name</span><input id="name" value="${esc(s.name)}"/></label>
    <label class="field"><span>Script</span><input id="script" value="${esc(s.script)}"/></label>
    <div class="hint">"name" and "script" are read by the backend on open_project.</div>
</section>

<section>
    <h2>Reliability</h2>
    <label class="check"><input id="auto_respawn" type="checkbox" ${checked(s.auto_respawn)}/> Auto-respawn backend on crash (rate-limited 5/min)</label>
    <label class="field"><span>Watchdog (ms)</span><input id="watchdog_ms" type="number" min="0" max="600000" value="${esc(s.watchdog_ms)}"/></label>
    <div class="hint">0 = disabled. When non-zero, every <code>inspect()</code> call has this many ms of wall-clock budget; runaway scripts are terminated.</div>
</section>

<section>
    <h2>Dispatch / Parallelism</h2>
    <div class="hint" style="margin-top:0">Per-group worker lanes. <b>No groups = legacy single pool.</b>
      Each group owns <code>max_parallel</code> threads at its OS priority (optionally pinned to cores),
      with its own queue + rate limit + result ordering. Applies on the next open / start.</div>
    <div id="par-groups"></div>
    <div class="row-buttons"><button id="par-add" class="secondary">＋ Add group</button></div>
    <label class="field" style="margin-top:8px"><span>Default group</span><select id="par-default"></select></label>
    <div class="subhead">Source → group (instance.json)</div>
    <div id="inst-groups" class="inst-grid"></div>
    <div class="subhead">Runtime — applies live (no restart)</div>
    <label class="field"><span>Process priority</span><select id="rt-priority">${prioOpt('')}</select></label>
    <label class="field"><span>Timer fps</span><input id="rt-timer" type="number" min="0" max="240" value="${esc(rt.timer_fps >= 0 ? rt.timer_fps : '')}" placeholder="(default 10)"/></label>
    <div class="hint">Timer fps = synthetic-tick rate; <b>0 = trigger-only</b> (your sources drive, no heartbeat).
      Process priority = the backend process vs the rest of the machine; <code>realtime</code> can starve the OS.
      Both apply <b>immediately</b> on change and save into <code>project.json</code> <code>runtime</code>.</div>
</section>

<section>
    <h2>C++ Toolchain</h2>
    <div id="tc-rows" class="hint">Checking toolchain…</div>
    <div class="hint">Resolved per project: <b>override</b> (saved here) → environment variable → built-in probe.
      Required items (xi headers, OpenCV, MSVC) warn in red if missing; libjpeg-turbo and IPP are optional accelerators.
      "Set path…" pins the path into this project's <code>project.json</code>; recompile to apply.</div>
</section>

<div class="row-buttons">
    <button id="save">Save</button>
    <span id="saved" class="saved">✓ saved</span>
</div>

<script>
const vscode = acquireVsCodeApi();

// ===== Dispatch / Parallelism editor (dynamic) =====
const PRIORITIES = ['high','normal','low'];
const OVERFLOWS  = ['drop_oldest','drop_newest','block'];
const ORDERS     = ['completion','arrival'];
let par = ${parJson};
const SOURCES = ${sourcesJson};
let instGroups = ${instGroupsJson};
par.groups = Array.isArray(par.groups) ? par.groups : [];

function affToStr(a) {
    if (!Array.isArray(a) || !a.length) return '';
    return JSON.stringify(a).slice(1, -1);   // [0,1,2,3] -> "0,1,2,3"; [[0,1],[2,3]] -> "[0,1],[2,3]"
}
function strToAff(v) {
    v = (v || '').trim();
    if (!v) return [];
    try { const x = JSON.parse('[' + v + ']'); return x; } catch { return []; }
}
function opt(list, cur) { return list.map(o => '<option ' + (o===cur?'selected':'') + '>' + o + '</option>').join(''); }

function renderGroups() {
    const box = document.getElementById('par-groups');
    box.innerHTML = '';
    par.groups.forEach((g, i) => {
        const d = document.createElement('div'); d.className = 'grp';
        d.innerHTML =
          '<div class="grp-head"><input class="gname" data-i="'+i+'" data-k="name" value="'+(g.name||'')+'" placeholder="group name"/>' +
          '<span class="spacer"></span><button class="secondary grp-x" data-del="'+i+'">✕ remove</button></div>' +
          '<div class="grp-grid">' +
          '<label>max_parallel <input type="number" min="1" max="32" data-i="'+i+'" data-k="max_parallel" value="'+(g.max_parallel??1)+'"/></label>' +
          '<label>thread_priority <select data-i="'+i+'" data-k="thread_priority">'+opt(PRIORITIES, g.thread_priority||'normal')+'</select></label>' +
          '<label>queue_depth <input type="number" min="1" max="10000" data-i="'+i+'" data-k="queue_depth" value="'+(g.queue_depth??100)+'"/></label>' +
          '<label>overflow <select data-i="'+i+'" data-k="overflow">'+opt(OVERFLOWS, g.overflow||'drop_oldest')+'</select></label>' +
          '<label>result_order <select data-i="'+i+'" data-k="result_order">'+opt(ORDERS, g.result_order||'completion')+'</select></label>' +
          '<label>min_interval_ms <input type="number" min="0" max="3600000" data-i="'+i+'" data-k="min_interval_ms" value="'+(g.min_interval_ms??0)+'"/></label>' +
          '<label>cpu_affinity <input data-i="'+i+'" data-k="cpu_affinity" value="'+affToStr(g.cpu_affinity)+'" placeholder="e.g. 0,1,2,3  (blank = unbound)"/></label>' +
          '</div>';
        box.appendChild(d);
    });
    box.querySelectorAll('[data-k]').forEach(el => {
        el.addEventListener('change', () => {
            const i = +el.dataset.i, k = el.dataset.k;
            if (k === 'name') par.groups[i].name = el.value.trim();
            else if (k === 'cpu_affinity') par.groups[i].cpu_affinity = strToAff(el.value);
            else if (el.type === 'number') par.groups[i][k] = parseInt(el.value || '0', 10);
            else par.groups[i][k] = el.value;
            if (k === 'name') { renderDefault(); renderInstGroups(); }
        });
    });
    box.querySelectorAll('[data-del]').forEach(b => b.addEventListener('click', () => {
        par.groups.splice(+b.dataset.del, 1); renderGroups(); renderDefault(); renderInstGroups();
    }));
}
function groupNames() { return par.groups.map(g => g.name).filter(Boolean); }
function renderDefault() {
    const s = document.getElementById('par-default');
    const names = groupNames();
    s.innerHTML = '<option value="">(first group)</option>' +
        names.map(n => '<option ' + (n===par.default_group?'selected':'') + '>'+n+'</option>').join('');
}
function renderInstGroups() {
    const box = document.getElementById('inst-groups');
    box.innerHTML = '';
    if (!SOURCES.length) { box.innerHTML = '<div class="hint" style="grid-column:1/3">No instances yet.</div>'; return; }
    const names = groupNames();
    SOURCES.forEach(n => {
        const lbl = document.createElement('div'); lbl.className = 'iname'; lbl.textContent = n;
        const selEl = document.createElement('select'); selEl.dataset.inst = n;
        selEl.innerHTML = '<option value="">(default)</option>' +
            names.map(g => '<option ' + (g===instGroups[n]?'selected':'') + '>'+g+'</option>').join('');
        selEl.addEventListener('change', () => { instGroups[n] = selEl.value; });
        box.append(lbl, selEl);
    });
}
document.getElementById('par-add').addEventListener('click', () => {
    par.groups.push({ name: 'group' + (par.groups.length+1), max_parallel: 1, thread_priority: 'normal',
        queue_depth: 100, overflow: 'drop_oldest', result_order: 'completion', cpu_affinity: [], min_interval_ms: 0 });
    renderGroups(); renderDefault(); renderInstGroups();
});
document.getElementById('par-default').addEventListener('change', e => { par.default_group = e.target.value; });
renderGroups(); renderDefault(); renderInstGroups();

// Runtime knobs — apply live (backend command) on change, persist on Save.
const RT = ${rtJson};
const rtPri = document.getElementById('rt-priority');
const rtTimer = document.getElementById('rt-timer');
rtPri.addEventListener('change', () => vscode.postMessage({ type: 'rt_priority', value: rtPri.value }));
rtTimer.addEventListener('change', () => {
    const v = rtTimer.value.trim();
    if (v !== '') vscode.postMessage({ type: 'rt_timer_fps', value: parseInt(v, 10) });
});

// ===== C++ toolchain health (unchanged) =====
function renderToolchain(h) {
    const box = document.getElementById('tc-rows');
    if (!h || !Array.isArray(h.components)) { box.textContent = 'Toolchain status unavailable.'; return; }
    box.innerHTML = '';
    for (const c of h.components) {
        const row = document.createElement('div'); row.className = 'tc-row';
        const naOptional = c.optional && !c.exists && c.source !== 'override';
        const badge = document.createElement('span');
        badge.className = 'tc-badge ' + (c.ok ? (naOptional ? 'tc-na' : 'tc-ok') : 'tc-warn');
        badge.textContent = c.ok ? (naOptional ? '○' : '●') : '▲';
        const label = document.createElement('span'); label.className = 'tc-label';
        label.innerHTML = c.label + (c.optional ? ' <em>(optional)</em>' : '');
        const pathSpan = document.createElement('span'); pathSpan.className = 'tc-path';
        pathSpan.textContent = c.path || '(not set)';
        if (c.path) { const sp = document.createElement('span'); sp.className = 'tc-src'; sp.textContent = c.source; pathSpan.appendChild(sp); }
        const actions = document.createElement('span'); actions.className = 'tc-actions';
        const setBtn = document.createElement('button'); setBtn.textContent = 'Set path…';
        setBtn.onclick = () => vscode.postMessage({ type: 'tc_set', key: c.key, isFile: c.key === 'vcvars' });
        actions.appendChild(setBtn);
        if (c.source === 'override') {
            const clr = document.createElement('button'); clr.textContent = 'Clear'; clr.className = 'secondary';
            clr.onclick = () => vscode.postMessage({ type: 'tc_clear', key: c.key });
            actions.appendChild(clr);
        }
        row.append(badge, label, pathSpan, actions);
        box.appendChild(row);
        if (c.hint) { const hint = document.createElement('div'); hint.className = 'tc-hint' + (c.ok ? ' muted' : ''); hint.textContent = c.hint; box.appendChild(hint); }
    }
}
vscode.postMessage({ type: 'tc_refresh' });

function collect() {
    // Parallelism: only emit if at least one named group; drop empty-named groups.
    const groups = par.groups.filter(g => (g.name || '').trim());
    const parallelism = groups.length ? { default_group: par.default_group || '', groups } : null;
    const inst = {}; for (const k of Object.keys(instGroups)) if (instGroups[k]) inst[k] = instGroups[k];
    return {
        name:         document.getElementById('name').value.trim(),
        script:       document.getElementById('script').value.trim(),
        auto_respawn: document.getElementById('auto_respawn').checked,
        watchdog_ms:  parseInt(document.getElementById('watchdog_ms').value || '0', 10),
        parallelism,
        instance_groups: inst,
        runtime: {
            process_priority: rtPri.value,
            timer_fps: rtTimer.value.trim() === '' ? -1 : parseInt(rtTimer.value, 10),
        },
    };
}
document.getElementById('save').addEventListener('click', () => {
    vscode.postMessage({ type: 'save', data: collect() });
});
window.addEventListener('message', e => {
    if (e.data?.type === 'saved') {
        const el = document.getElementById('saved');
        el.classList.add('show');
        setTimeout(() => el.classList.remove('show'), 1500);
    } else if (e.data?.type === 'tc_health') {
        renderToolchain(e.data.data);
    }
});
</script>
</body></html>`;
}
