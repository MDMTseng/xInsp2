// Multi-camera correlation test:
//   1. Spawn backend
//   2. Create project + synced_stereo instance "synced0"
//   3. Compile a script that reads xi::current_trigger().image("synced0/left|right")
//      and stores the embedded seq numbers from each frame as VARs
//   4. Start the source; let it tick for a beat
//   5. Run the inspect manually a couple of times via 'run' AND let
//      continuous mode dispatch on bus events
//   6. Read back the captured vars and verify left.seq == right.seq for
//      every dispatched event (the correlation guarantee)
//
import { spawn } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { writeFileSync } from 'node:fs';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const exe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const port = 30000 + Math.floor(Math.random() * 20000);

const backend = spawn(exe, [`--port=${port}`], { stdio: ['ignore', 'pipe', 'pipe'] });
backend.stderr.on('data', d => process.stderr.write(d));
await new Promise(r => setTimeout(r, 2500));

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
const handlers = new Map();
let varEvents = [];
ws.on('message', (data, isBinary) => {
    if (isBinary) return;            // preview frames — ignore
    const text = data.toString();
    if (!text || text[0] !== '{') return;
    let m; try { m = JSON.parse(text); } catch { return; }
    if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
    else if (m.type === 'vars') varEvents.push(m);
});
function send(name, args) {
    return new Promise(res => {
        const id = Math.floor(Math.random() * 1e9);
        handlers.set(id, res);
        ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
    });
}

console.log('--- create project + instance ---');
const projDir = resolve(tmpdir(), `multicam_${Date.now()}`);
console.log(JSON.stringify((await send('create_project', { folder: projDir, name: 'multicam' })).ok));
console.log('create_instance:',
    JSON.stringify((await send('create_instance', { name: 'synced0', plugin: 'synced_stereo' })).ok));

console.log('--- write inspection.cpp using xi::current_trigger() ---');
const scriptPath = resolve(projDir, 'inspection.cpp');
writeFileSync(scriptPath, `
#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto t = xi::current_trigger();
    VAR(active, t.is_active());
    if (!t.is_active()) return;

    VAR(tid, t.id_string());
    VAR(timestamp_us, (double)t.timestamp_us());

    auto left  = t.image("synced0/left");
    auto right = t.image("synced0/right");

    VAR(has_left,  !left.empty());
    VAR(has_right, !right.empty());

    if (!left.empty() && !right.empty()) {
        // Each frame has the seq number embedded in the first 4 bytes.
        int seqL = 0, seqR = 0;
        std::memcpy(&seqL, left.data(),  sizeof(int));
        std::memcpy(&seqR, right.data(), sizeof(int));
        VAR(seq_left, seqL);
        VAR(seq_right, seqR);
        VAR(matched, seqL == seqR);
    }
}
`);

console.log('--- compile ---');
const c = await send('compile_and_load', { path: scriptPath });
console.log('  ok=' + c.ok + (c.error ? ' error=' + c.error : ''));

console.log('--- start synced_stereo + 8 fps ---');
await send('exchange_instance', { name: 'synced0', cmd: { command: 'set_fps', value: 8 } });
await send('exchange_instance', { name: 'synced0', cmd: { command: 'start' } });

console.log('--- start continuous → 2 seconds of bus-driven dispatches ---');
await send('start', { fps: 8 });
await new Promise(r => setTimeout(r, 2200));
await send('stop');
console.log(`captured ${varEvents.length} vars events`);
if (varEvents.length > 0) {
    console.log('first event keys:', Object.keys(varEvents[0]));
    console.log('first event vars:', JSON.stringify(varEvents[0]).slice(0, 600));
}

console.log('--- correlation summary ---');
const flatten = (ev) => {
    const out = {};
    for (const it of (ev.items || [])) out[it.name] = it.value;
    return out;
};
let total = 0, matched = 0, mismatched = 0;
let unique_tids = new Set();
for (const ev of varEvents) {
    const v = flatten(ev);
    if (v.matched === undefined) continue;
    total++;
    if (v.tid) unique_tids.add(v.tid);
    if (v.matched === true) matched++;
    else mismatched++;
}
console.log(`  total triggers with both frames: ${total}`);
console.log(`  unique trigger IDs:              ${unique_tids.size}`);
console.log(`  matched (left.seq == right.seq):  ${matched}`);
console.log(`  mismatched:                       ${mismatched}`);

// Show first 3 sample events
const samples = varEvents.map(flatten).filter(v => v.matched !== undefined).slice(0, 3);
for (const v of samples) {
    console.log(`  tid=${(v.tid || '').slice(0, 16)}…  seq_left=${v.seq_left}  seq_right=${v.seq_right}  matched=${v.matched}`);
}

ws.close();
backend.kill();
await new Promise(r => setTimeout(r, 500));

if (total === 0) { console.error('FAIL: no correlated triggers seen'); process.exit(1); }
if (mismatched > 0) { console.error(`FAIL: ${mismatched} mismatched`); process.exit(1); }
console.log(`\nOK: ${matched}/${total} triggers correlated correctly`);
