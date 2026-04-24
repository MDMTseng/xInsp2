// runTriggerPolicies.mjs — exercise all three TriggerBus policies.
//
// Two synced_stereo instances emit at 8fps each. They generate fresh,
// independent 128-bit tids per tick, so their emit streams never
// correlate across sources.
//
// Policy semantics under test:
//   Any              — dispatch on every emit (both sources fire)
//   AllRequired req=[synced0,synced1] — dispatch only when both have the
//                     same tid; with independent tids, never fires
//   LeaderFollowers leader=synced0   — dispatch on synced0's emits;
//                     synced1 contributes best-effort latest frames
//
// We count inspect() runs during a 2s continuous window for each policy
// and assert the qualitative relationship holds.
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
let varsEvents = [];
ws.on('message', (data, isBinary) => {
    if (isBinary) return;
    const t = data.toString();
    if (t[0] !== '{') return;
    try {
        const m = JSON.parse(t);
        if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
        else if (m.type === 'vars') varsEvents.push(m);
    } catch {}
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});

// ---- Stage project with 2 synced_stereo instances --------------------
const projDir = resolve(tmpdir(), `trig_${Date.now()}`);
await send('create_project', { folder: projDir, name: 'trig_policies' });
await send('create_instance', { name: 'synced0', plugin: 'synced_stereo' });
await send('create_instance', { name: 'synced1', plugin: 'synced_stereo' });
await send('exchange_instance', { name: 'synced0', cmd: { command: 'set_fps', value: 8 } });
await send('exchange_instance', { name: 'synced1', cmd: { command: 'set_fps', value: 8 } });

const script = join(projDir, 'inspection.cpp');
writeFileSync(script, `
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto t = xi::current_trigger();
    if (!t.is_active()) return;
    VAR(tid, t.id_string());
    VAR(has_s0_left,  !t.image("synced0/left").empty());
    VAR(has_s0_right, !t.image("synced0/right").empty());
    VAR(has_s1_left,  !t.image("synced1/left").empty());
    VAR(has_s1_right, !t.image("synced1/right").empty());
}
`);
const cr = await send('compile_and_load', { path: script });
if (!cr.ok) { console.error('compile failed:', cr.error); process.exit(2); }

let failed = 0;
function check(cond, label) { if (cond) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

async function runOnePolicy(policy, extra) {
    await send('set_trigger_policy', { policy, ...extra });
    await send('exchange_instance', { name: 'synced0', cmd: { command: 'start' } });
    await send('exchange_instance', { name: 'synced1', cmd: { command: 'start' } });
    varsEvents = [];
    await send('start', { fps: 30 });
    await new Promise(r => setTimeout(r, 2000));
    await send('stop');
    await send('exchange_instance', { name: 'synced0', cmd: { command: 'stop' } });
    await send('exchange_instance', { name: 'synced1', cmd: { command: 'stop' } });
    // Drain
    await new Promise(r => setTimeout(r, 400));
    return varsEvents.slice();   // copy
}
function flatten(ev) {
    const o = {};
    for (const it of (ev.items || [])) o[it.name] = it.value;
    return o;
}
function countEventsWith(list, pred) {
    return list.filter(ev => pred(flatten(ev))).length;
}

// Continuous mode has a 33ms TIMER fallback alongside the bus. Timer
// dispatches fire inspect() with no current_trigger, so our script
// early-returns and the vars snapshot has no `tid`. Only bus-driven
// dispatches produce a `tid` var — that's what we count.
const busDriven = (list) => list.map(flatten).filter(v => v.tid);

// ---- Any: every emit from either source triggers a dispatch -----------
console.log('\n--- policy=any ---');
let ev = await runOnePolicy('any', {});
let bus = busDriven(ev);
let s0Only = bus.filter(v => v.has_s0_left === true && v.has_s1_left !== true).length;
let s1Only = bus.filter(v => v.has_s1_left === true && v.has_s0_left !== true).length;
console.log(`  bus-dispatches=${bus.length}  s0-only=${s0Only}  s1-only=${s1Only}`);
check(bus.length >= 20, `>=20 bus dispatches in 2s from 2 sources at 8fps (got ${bus.length})`);
check(s0Only > 0 && s1Only > 0, 'Any fires events from BOTH sources independently');

// ---- AllRequired: tids never align → zero bus-driven dispatches -------
console.log('\n--- policy=all_required (both sources, tids never align) ---');
ev = await runOnePolicy('all_required', {
    required: ['synced0', 'synced1'],
    window_ms: 30,
});
bus = busDriven(ev);
console.log(`  bus-dispatches=${bus.length}  (timer-only vars events ignored)`);
check(bus.length === 0,
    `AllRequired with misaligned tids dispatches 0 bus events (got ${bus.length})`);

// ---- LeaderFollowers: leader synced0 drives; synced1 best-effort ------
console.log('\n--- policy=leader_followers (leader=synced0) ---');
ev = await runOnePolicy('leader_followers', {
    leader: 'synced0',
    required: ['synced1'],
});
bus = busDriven(ev);
const leaderHits   = bus.filter(v => v.has_s0_left === true).length;
const followerHits = bus.filter(v => v.has_s1_left === true).length;
console.log(`  bus-dispatches=${bus.length}  leader-present=${leaderHits}  follower-present=${followerHits}`);
check(leaderHits >= 10, `>=10 dispatches with leader frames (got ${leaderHits})`);
check(bus.length === leaderHits, 'LeaderFollowers dispatches ONLY on leader emit (all bus events carry synced0)');
check(followerHits > 0, 'follower frames attached at least once (best-effort)');

ws.close();
backend.kill();
await new Promise(r => setTimeout(r, 500));
if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: all three trigger policies behave per spec');
