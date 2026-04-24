// runBreakpoint.mjs — S3 xi::breakpoint() flow.
//
// Script hits 2 breakpoints per frame. We assert:
//   - breakpoint event arrives with the right label
//   - the inspection stays parked (no further vars/events) until resume
//   - resume responds with {resumed:true}; second resume replies {resumed:false}
//   - stop-while-paused doesn't deadlock (auto-release on join)
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
const events = [];
const varsEvents = [];
ws.on('message', (data, isBinary) => {
    if (isBinary) return;
    const t = data.toString();
    if (t[0] !== '{') return;
    try {
        const m = JSON.parse(t);
        if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
        else if (m.type === 'event') events.push(m);
        else if (m.type === 'vars') varsEvents.push(m);
        else if (m.type === 'log' && m.level === 'error') console.error('[log]', m.msg?.slice(0, 800));
    } catch {}
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// --- stage: project + script with 2 breakpoints -------------------------
const projDir = resolve(tmpdir(), `bp_${Date.now()}`);
await send('create_project', { folder: projDir, name: 'bp' });
const script = join(projDir, 'inspection.cpp');
writeFileSync(script, `
#include <xi/xi.hpp>
#include <xi/xi_breakpoint.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(a, 1);
    xi::breakpoint("after_a");
    VAR(b, 2);
    xi::breakpoint("after_b");
    VAR(c, 3);
}
`);
const cr = await send('compile_and_load', { path: script });
if (!cr.ok) { console.error('compile:', cr.error); process.exit(2); }

let failed = 0;
function check(cond, label) { if (cond) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

// --- Start continuous mode (breakpoints only effective there) ---------
events.length = 0; varsEvents.length = 0;
await send('start', { fps: 4 });
await sleep(400);                                 // let the worker park

let bp1 = events.find(e => e.name === 'breakpoint' && e.data?.label === 'after_a');
check(!!bp1, 'event breakpoint {label:"after_a"} received');

// While paused at first breakpoint, NO further vars should appear.
const varsBeforeResume = varsEvents.length;
await sleep(400);
check(varsEvents.length === varsBeforeResume,
    `no new vars while paused (got ${varsEvents.length - varsBeforeResume})`);

// Resume #1 — should release to second breakpoint
let r = await send('resume');
check(r.ok && r.data?.resumed === true && r.data?.label === 'after_a', 'resume #1 returns {resumed:true,"after_a"}');
await sleep(300);

let bp2 = events.find(e => e.name === 'breakpoint' && e.data?.label === 'after_b');
check(!!bp2, 'event breakpoint {label:"after_b"} received after first resume');

// Resume #2 — script finishes; vars are emitted
const varsBefore2 = varsEvents.length;
r = await send('resume');
check(r.ok && r.data?.resumed === true && r.data?.label === 'after_b', 'resume #2 returns {resumed:true,"after_b"}');
await sleep(300);
check(varsEvents.length > varsBefore2, 'vars arrived after final resume');

// Stop-while-paused: at 4fps the worker will be parked again at another
// breakpoint. Stop must not deadlock on join — it should auto-release
// the parked script.
await sleep(200);
const st = await send('stop');
check(st.ok, 'stop while possibly paused succeeds (no deadlock)');

// With no continuous mode running, nobody can be paused anymore.
r = await send('resume');
check(r.ok && r.data?.resumed === false, 'resume after stop → {resumed:false}');

ws.close();
backend.kill();
await sleep(500);
if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: xi::breakpoint round-trip works');
