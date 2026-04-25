// runHistory.mjs — S4 history ring buffer.
//
// Run inspect 8 times with frame number captured as a VAR. Then:
//   - history default depth >= 8, ring contains all 8
//   - history {count:3} returns the 3 newest
//   - newest-first ordering via run_id
//   - set_history_depth {depth:5} truncates to 5 newest
//   - since_run_id filter only returns newer runs
//
import { spawn } from 'node:child_process';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { writeFileSync } from 'node:fs';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const exe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const port = 40000 + Math.floor(Math.random() * 20000);
const backend = spawn(exe, [`--port=${port}`], { stdio: ['ignore', 'pipe', 'pipe'] });
backend.stderr.on('data', d => process.stderr.write(`[be] ${d}`));
await new Promise(r => setTimeout(r, 2500));

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
const handlers = new Map();
ws.on('message', (d, isBin) => {
    if (isBin) return;
    const t = d.toString();
    if (t[0] !== '{') return;
    try {
        const m = JSON.parse(t);
        if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
    } catch {}
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// --- stage script that emits a tagged var per run ---------------------
const projDir = resolve(tmpdir(), `hist_${Date.now()}`);
await send('create_project', { folder: projDir, name: 'hist' });
const script = join(projDir, 'inspection.cpp');
writeFileSync(script, `
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(tag, frame);
}
`);
const cr = await send('compile_and_load', { path: script });
if (!cr.ok) { console.error('compile failed:', cr.error); process.exit(2); }

// 8 sequential runs
for (let i = 0; i < 8; ++i) await send('run');
await sleep(200);

let failed = 0;
function check(c, label) { if (c) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

const tag = (run) => {
    const it = (run.vars || []).find(v => v.name === 'tag');
    return it?.value;
};

// Default request
let r = await send('history');
check(r.ok, 'history call ok');
check(r.data.depth === 50, 'default depth = 50');
check(r.data.size === 8, `ring size after 8 runs = 8 (got ${r.data.size})`);
check(r.data.runs.length === 8, '8 runs returned');
// Newest first: run_id descending
const ids = r.data.runs.map(x => x.run_id);
const sorted = [...ids].sort((a, b) => b - a);
check(JSON.stringify(ids) === JSON.stringify(sorted), `runs newest-first (got ${ids.join(',')})`);

// count = 3 returns newest 3
r = await send('history', { count: 3 });
check(r.data.runs.length === 3, '{count:3} returns 3 runs');
check(r.data.runs[0].run_id > r.data.runs[2].run_id, 'newest first under count');

// resize to 5
r = await send('set_history_depth', { depth: 5 });
check(r.ok && r.data.depth === 5, 'set_history_depth=5 accepted');
r = await send('history');
check(r.data.size === 5, `ring trimmed to 5 (got ${r.data.size})`);
check(r.data.runs.length === 5, 'history returns 5');
const oldestKept = r.data.runs[r.data.runs.length - 1].run_id;

// since_run_id filter
r = await send('history', { since_run_id: oldestKept });
// Should include all runs strictly newer than oldestKept => 4 entries
check(r.data.runs.length === 4, `since_run_id=${oldestKept} returns 4 newer (got ${r.data.runs.length})`);
check(r.data.runs.every(x => x.run_id > oldestKept), 'all returned ids > since');

// One more run, verify it appears
await send('run');
await sleep(100);
r = await send('history', { count: 1 });
check(r.data.runs.length === 1, 'single newest after another run');
check(typeof tag(r.data.runs[0]) === 'number', 'newest carries the tag VAR');

ws.close();
backend.kill();
await sleep(500);
if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: history ring buffer behaves per spec');
