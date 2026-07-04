// runWatchdog.mjs — P2.4 inspect-loop watchdog (per-worker, exit-on-hard-trip).
//
// Contract (changed 2026-05): a runaway inspect that ignores cooperative cancel
// no longer gets TerminateThread'd-and-continued (that would leak the per-instance
// lock + risk heap corruption). Instead the backend EXITS with WATCHDOG_EXIT_CODE
// so the FE supervisor respawns a clean one. This test compiles a `while(1){}`
// script (never polls cancel), sets watchdog 500 ms, fires a run, and verifies the
// backend exits with the right code and logs the hard trip.
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
const WATCHDOG_EXIT_CODE = 0x5744;   // 'WD' — must match service_main.cpp

// Boot backend with --watchdog=500. Capture stderr so we can assert the trip log.
const backend = spawn(exe, [`--port=${port}`, '--watchdog=500'],
                      { stdio: ['ignore', 'pipe', 'pipe'] });
let beErr = '';
backend.stderr.on('data', d => { beErr += d.toString(); process.stderr.write(`[be] ${d}`); });
let exitCode = null;
const exited = new Promise(res => backend.on('exit', (code) => { exitCode = code; res(code); }));
await new Promise(r => setTimeout(r, 2500));

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
const handlers = new Map();
ws.on('message', (data, isBin) => {
    if (isBin) return;
    const t = data.toString();
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

// --- stage script with infinite loop (never polls cancel) ------------
const projDir = resolve(tmpdir(), `wd_${Date.now()}`);
await send('create_project', { folder: projDir, name: 'wd' });
const script = join(projDir, 'inspection.cpp');
writeFileSync(script, `
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    (void)frame;
    volatile long long sink = 0;
    while (true) sink = sink + 1;   // never returns, never checks cancel
}
`);
const cr = await send('compile_and_load', { path: script });
if (!cr.ok) { console.error('compile failed:', cr.error); process.exit(2); }

let failed = 0;
function check(c, label) { if (c) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

// Watchdog snapshot before (backend still alive). `watchdog_status` was retired
// at THE CUT; `set_watchdog_ms` returns the same { ms, trips } snapshot, so a
// no-op re-set of the boot value doubles as the status read.
let s1 = await send('set_watchdog_ms', { ms: 500 });
check(s1.ok && s1.data.ms === 500, `--watchdog=500 picked up (got ${s1.data?.ms})`);
check(s1.data.trips === 0, 'no trips at start');

// Fire run — busy loop. Watchdog: 500 ms deadline + ~1000 ms cooperative grace,
// then HARD trip => backend exits. The run rsp never arrives.
const id = Math.floor(Math.random() * 1e9);
ws.send(JSON.stringify({ type: 'cmd', id, name: 'run' }));
console.log('  fired runaway run; waiting for watchdog hard trip + backend exit…');

// The backend should exit on its own within the grace window + margin.
const winner = await Promise.race([exited, sleep(8000).then(() => 'timeout')]);
check(winner !== 'timeout', 'backend exited on its own (did not hang)');
check(exitCode === WATCHDOG_EXIT_CODE,
      `backend exited with watchdog code 0x${WATCHDOG_EXIT_CODE.toString(16)} (got ${exitCode})`);
check(/watchdog HARD trip/.test(beErr), 'backend logged the hard trip');
// The watchdog must attempt a cooperative cancel BEFORE it hard-trips — a
// backend that jumps straight to the hard trip (skipping the soft-cancel
// escalation) is a regression. `|| true` here previously made this a no-op.
check(/cooperative cancel/.test(beErr), 'cooperative cancel attempted first');
check(beErr.indexOf('cooperative cancel') < beErr.indexOf('watchdog HARD trip'),
      'cooperative cancel logged before the hard trip');

try { ws.close(); } catch {}
try { backend.kill(); } catch {}
await sleep(300);
if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: runaway inspect -> watchdog hard trip -> backend exits for FE respawn');
