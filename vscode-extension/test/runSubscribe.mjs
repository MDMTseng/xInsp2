// runSubscribe.mjs — S1 live preview subscription.
//
// Exercises:
//   1. Default (send-all) behaviour: after run, count binary preview frames
//      matches image VARs in the script.
//   2. subscribe {names:['keep']}: only the one matching preview arrives.
//   3. unsubscribe: zero previews, but vars metadata still arrives.
//   4. subscribe {all:true}: back to all-previews.
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
let binCount = 0;
let varsSeen = 0;
ws.on('message', (data, isBinary) => {
    if (isBinary) { binCount++; return; }
    const t = data.toString();
    if (t[0] !== '{') return;
    try {
        const m = JSON.parse(t);
        if (m.type === 'rsp' && handlers.has(m.id)) { handlers.get(m.id)(m); handlers.delete(m.id); }
        if (m.type === 'vars') varsSeen++;
    } catch {}
});
const send = (name, args) => new Promise(res => {
    const id = Math.floor(Math.random() * 1e9);
    handlers.set(id, res);
    ws.send(JSON.stringify({ type: 'cmd', id, name, args }));
});

// ---- Stage a tiny project with 3 image VARs ---------------------------
const projDir = resolve(tmpdir(), `sub_${Date.now()}`);
await send('create_project', { folder: projDir, name: 'sub_test' });
const script = join(projDir, 'inspection.cpp');
writeFileSync(script, `
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::Image a(64, 64, 1); std::memset(a.data(), 10, 64*64);
    xi::Image b(64, 64, 1); std::memset(b.data(), 50, 64*64);
    xi::Image c(64, 64, 1); std::memset(c.data(), 90, 64*64);
    VAR(alpha, a);
    VAR(beta,  b);
    VAR(gamma, c);
    VAR(count, 3);
}
`);
const cr = await send('compile_and_load', { path: script });
if (!cr.ok) { console.error('compile failed:', cr.error); process.exit(2); }

let failed = 0;
function check(cond, label) { if (cond) console.log(`  ✓ ${label}`); else { console.log(`  ✗ ${label}`); failed++; } }

async function runAndCount() {
    binCount = 0; varsSeen = 0;
    await send('run');
    await new Promise(r => setTimeout(r, 500));
    return { bin: binCount, vars: varsSeen };
}

// Case 1: default send-all
let r = await runAndCount();
console.log(`--- default: ${r.bin} previews, ${r.vars} vars msgs ---`);
check(r.vars === 1, 'one vars message');
check(r.bin === 3,  '3 preview frames (alpha + beta + gamma)');

// Case 2: subscribe to {beta}
let sub = await send('subscribe', { names: ['beta'] });
check(sub.ok && sub.data.all === false && sub.data.count === 1, 'subscribe {beta} accepted');
r = await runAndCount();
console.log(`--- subscribe beta: ${r.bin} previews ---`);
check(r.bin === 1, 'only 1 preview frame (beta)');
check(r.vars === 1, 'vars message still arrives');

// Case 3: subscribe to multi {alpha, gamma}
await send('subscribe', { names: ['alpha', 'gamma'] });
r = await runAndCount();
console.log(`--- subscribe alpha+gamma: ${r.bin} previews ---`);
check(r.bin === 2, '2 preview frames (alpha + gamma)');

// Case 4: unsubscribe
let us = await send('unsubscribe');
check(us.ok && us.data.all === false && us.data.count === 0, 'unsubscribe → empty set');
r = await runAndCount();
console.log(`--- unsubscribe: ${r.bin} previews ---`);
check(r.bin === 0, '0 preview frames');
check(r.vars === 1, 'vars still arrives');

// Case 5: subscribe {all:true}
await send('subscribe', { all: true });
r = await runAndCount();
console.log(`--- subscribe all: ${r.bin} previews ---`);
check(r.bin === 3, 'all 3 preview frames again');

ws.close();
backend.kill();
await new Promise(r => setTimeout(r, 500));
if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: preview subscription gates binary frames correctly');
