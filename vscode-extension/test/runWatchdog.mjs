// runWatchdog.mjs — P2.4 inspect-loop watchdog.
//
// Compile a script with `while(1) {}` infinite loop; set watchdog 500 ms;
// run; verify the trip count went up and a log:error event arrived.
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

// Boot backend with --watchdog=500 (also set via cmd later for test).
const backend = spawn(exe, [`--port=${port}`, '--watchdog=500'],
                      { stdio: ['ignore', 'pipe', 'pipe'] });
backend.stderr.on('data', d => process.stderr.write(`[be] ${d}`));
await new Promise(r => setTimeout(r, 2500));

const ws = new WebSocket(`ws://127.0.0.1:${port}`);
await new Promise((res, rej) => { ws.once('open', res); ws.once('error', rej); });
const handlers = new Map();
const logs = [];
ws.on('message', (data, isBin) => {
    if (isBin) return;
    const t = data.toString();
    if (t[0] !== '{') return;
    try {
        const m = JSON.parse(t);
        if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
        else if (m.type === 'log') logs.push(m);
    } catch {}
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// --- stage script with infinite loop ---------------------------------
const projDir = resolve(tmpdir(), `wd_${Date.now()}`);
await send('create_project', { folder: projDir, name: 'wd' });
const script = join(projDir, 'inspection.cpp');
writeFileSync(script, `
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(start, frame);
    // Busy loop — never returns. Watchdog should terminate this thread.
    volatile int sink = 0;
    while (true) sink = sink + 1;
}
`);
const cr = await send('compile_and_load', { path: script });
if (!cr.ok) { console.error('compile failed:', cr.error); process.exit(2); }

let failed = 0;
function check(c, label) { if (c) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

// Watchdog status before
let s1 = await send('watchdog_status');
check(s1.ok && s1.data.ms === 500, `--watchdog=500 picked up (got ${s1.data?.ms})`);
check(s1.data.trips === 0, 'no trips at start');

// Trigger inspect — runs on WS thread synchronously. The watchdog should
// terminate the thread; the "run" rsp will not arrive (thread killed),
// so we simulate by firing a fire-and-forget run and then polling status.
const id = Math.floor(Math.random() * 1e9);
ws.send(JSON.stringify({ type: 'cmd', id, name: 'run' }));
console.log('  fired run; waiting for watchdog to trip…');
await sleep(2500);

// Status should now show trips >= 1
let s2 = await send('watchdog_status');
console.log(`  status after: ms=${s2.data?.ms} trips=${s2.data?.trips} armed=${s2.data?.armed}`);
check(s2.ok, 'status query works post-trip (backend alive)');
check(s2.data.trips >= 1, `watchdog tripped (got ${s2.data?.trips})`);
check(s2.data.armed === false, 'watchdog disarmed after trip');
const errLogs = logs.filter(l => l.level === 'error' && l.msg?.includes('watchdog'));
check(errLogs.length >= 1, `received watchdog error log (${errLogs.length})`);

// Backend still works for other commands
let pong = await send('ping');
check(pong.ok && pong.data.pong === true, 'backend still responsive after trip');

// Disable watchdog at runtime
let s3 = await send('set_watchdog_ms', { ms: 0 });
check(s3.ok && s3.data.ms === 0, 'watchdog disabled via cmd');

ws.close();
backend.kill();
await sleep(500);
if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: watchdog terminates runaway inspect, backend stays alive');
