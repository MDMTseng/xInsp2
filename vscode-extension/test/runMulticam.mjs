// Multi-camera correlation test (v12, pack plane):
//   1. Spawn backend
//   2. Create project + synced_stereo instance "synced0"
//   3. Compile a script that reads the gathered pack off the trigger
//      (t.pack(): images "left"/"right" + the "seq" entry) and verifies the
//      embedded seq numbers agree — the correlation guarantee
//   4. Start the source; let it tick for a beat under continuous mode
//   5. Read back the run_result verdicts (the vars frame is gone; the verdict
//      carries matched/mismatch per dispatched event)
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
let resultEvents = [];
ws.on('message', (data, isBinary) => {
    if (isBinary) return;            // preview frames — ignore
    const text = data.toString();
    if (!text || text[0] !== '{') return;
    let m; try { m = JSON.parse(text); } catch { return; }
    if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
    else if (m.type === 'event' && m.name === 'run_result') resultEvents.push(m.data);
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

console.log('--- write inspection.cpp reading the gathered pack ---');
const scriptPath = resolve(projDir, 'inspection.cpp');
writeFileSync(scriptPath, `
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
#include <cstring>

XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;                 // synthetic tick, nothing to check
    auto f = t.pack();
    if (!f) { xi::result(-3, "no pack"); return; }
    auto left  = f.get_image("left");
    auto right = f.get_image("right");
    if (!left || !right) { xi::result(-2, "missing image"); return; }

    // Each frame has the seq number embedded in the first 4 bytes; the pack
    // also carries it as the "seq" entry — all three must agree.
    int seqL = 0, seqR = 0;
    std::memcpy(&seqL, left->pixels.data(),  sizeof(int));
    std::memcpy(&seqR, right->pixels.data(), sizeof(int));
    long long seq = f.get_i64("seq").value_or(-1);
    bool matched = (seqL == seqR) && (seqL == (int)seq);
    xi::result(matched ? (seqL + 1) : -1,
               matched ? ("matched:" + t.id_string()) : "mismatch");
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
await send('exchange_instance', { name: 'synced0', cmd: { command: 'stop' } });
await send('stop');
console.log(`captured ${resultEvents.length} run_result events`);
if (resultEvents.length > 0) {
    console.log('first event:', JSON.stringify(resultEvents[0]).slice(0, 300));
}

console.log('--- correlation summary ---');
let total = 0, matched = 0, mismatched = 0;
let unique_tids = new Set();
for (const r of resultEvents) {
    const msg = String(r.msg || '');
    if (!msg.startsWith('matched') && msg !== 'mismatch') continue;  // ticks etc.
    total++;
    if (msg.startsWith('matched:')) { matched++; unique_tids.add(msg.slice(8)); }
    else mismatched++;
}
console.log(`  total triggers with both frames: ${total}`);
console.log(`  unique trigger IDs:              ${unique_tids.size}`);
console.log(`  matched (left.seq == right.seq):  ${matched}`);
console.log(`  mismatched:                       ${mismatched}`);

// Show first 3 sample events
for (const r of resultEvents.filter(x => String(x.msg || '').startsWith('matched')).slice(0, 3)) {
    console.log(`  tid=${String(r.msg).slice(8, 24)}…  seq+1=${r.code}`);
}

ws.close();
backend.kill();
await new Promise(r => setTimeout(r, 500));

if (total === 0) { console.error('FAIL: no correlated triggers seen'); process.exit(1); }
if (mismatched > 0) { console.error(`FAIL: ${mismatched} mismatched`); process.exit(1); }
console.log(`\nOK: ${matched}/${total} triggers correlated correctly`);
