// Record & replay end-to-end:
//   1. Spawn backend, create project, add synced_stereo instance
//   2. Start the source + recording_start
//   3. Let it run for ~1.5s
//   4. recording_stop → manifest written
//   5. Inspect the recording dir on disk
//   6. recording_replay → events stream back through the bus; verify the
//      script (with current_trigger awareness) sees the same seq numbers
//
import { spawn } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { writeFileSync, readdirSync, readFileSync, existsSync } from 'node:fs';
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
const varEvents = [];
ws.on('message', (data, isBinary) => {
    if (isBinary) return;
    const text = data.toString();
    if (!text || text[0] !== '{') return;
    let m; try { m = JSON.parse(text); } catch { return; }
    if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
    else if (m.type === 'vars') varEvents.push(m);
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});

const projDir = resolve(tmpdir(), `replay_${Date.now()}`);
const recDir  = resolve(projDir, 'recording');
console.log('--- setup ---');
console.log('  create_project:', (await send('create_project', { folder: projDir, name: 'replay' })).ok);
console.log('  create_instance synced_stereo:', (await send('create_instance', { name: 'synced0', plugin: 'synced_stereo' })).ok);

const scriptPath = resolve(projDir, 'inspection.cpp');
writeFileSync(scriptPath, `
#include <xi/xi.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;
    auto left  = t.image("synced0/left");
    auto right = t.image("synced0/right");
    if (left.empty() || right.empty()) return;
    int seqL = 0, seqR = 0;
    std::memcpy(&seqL, left.data(),  sizeof(int));
    std::memcpy(&seqR, right.data(), sizeof(int));
    VAR(tid, t.id_string());
    VAR(seq_left,  seqL);
    VAR(seq_right, seqR);
    VAR(matched,   seqL == seqR);
}
`);
console.log('  compile:', (await send('compile_and_load', { path: scriptPath })).ok);

console.log('\n--- recording phase ---');
await send('exchange_instance', { name: 'synced0', cmd: { command: 'set_fps', value: 8 } });
await send('exchange_instance', { name: 'synced0', cmd: { command: 'start' } });
console.log('  recording_start:', (await send('recording_start', { folder: recDir })).ok);
await send('start', { fps: 8 });          // continuous mode → script captures vars
await new Promise(r => setTimeout(r, 1500));
await send('stop');
await send('exchange_instance', { name: 'synced0', cmd: { command: 'stop' } });
const stopRsp = await send('recording_stop');
console.log('  recording_stop:', JSON.stringify(stopRsp.data));

console.log('\n--- on-disk inspection ---');
const files = readdirSync(recDir);
console.log(`  files in ${recDir}: ${files.length}`);
console.log(`  examples: ${files.slice(0, 5).join(', ')}`);
const manifest = JSON.parse(readFileSync(resolve(recDir, 'manifest.json'), 'utf8'));
console.log(`  manifest events: ${manifest.events.length}`);

const liveCount = varEvents.length;
console.log(`  live vars events captured: ${liveCount}`);
varEvents.length = 0;        // clear for replay phase

console.log('\n--- replay phase ---');
console.log('  recording_replay:', (await send('recording_replay', { folder: recDir, speed: 4.0 })).ok);
// Continuous mode is now stopped — but emit_trigger via replay still
// fires the observer if any. To run inspect on replayed events we need
// continuous mode running too.
await send('start', { fps: 30 });
await new Promise(r => setTimeout(r, 2000));
await send('stop');
const replayCount = varEvents.length;
console.log(`  replayed vars events: ${replayCount}`);

// Validate: every replayed event should have matching seqs
const flatten = (ev) => {
    const out = {}; for (const it of (ev.items || [])) out[it.name] = it.value; return out;
};
let total = 0, matched = 0;
const replay_tids = new Set();
for (const ev of varEvents) {
    const v = flatten(ev);
    if (v.matched !== undefined) {
        total++;
        if (v.matched === true) matched++;
        if (v.tid) replay_tids.add(v.tid);
    }
}
console.log(`  replay events with matched vars: ${matched}/${total}`);
console.log(`  unique tids in replay: ${replay_tids.size}`);
console.log(`  manifest event count vs replay tids: ${manifest.events.length} vs ${replay_tids.size}`);

ws.close();
backend.kill();
await new Promise(r => setTimeout(r, 500));

if (manifest.events.length === 0) { console.error('FAIL: nothing recorded'); process.exit(1); }
if (total === 0) { console.error('FAIL: replay produced 0 events'); process.exit(1); }
if (matched !== total) { console.error('FAIL: replayed events not correlated'); process.exit(1); }
console.log('\nOK: record + replay round-trip works');
