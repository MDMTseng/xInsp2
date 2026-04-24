// Remote-backend + shared-secret auth regression.
//
// Spawns the backend bound to 0.0.0.0 with a secret, then runs four cases
// against it:
//   1. loopback connect WITHOUT the Authorization header     → denied (401)
//   2. loopback connect WITH wrong Bearer                    → denied (401)
//   3. loopback connect WITH correct Bearer                  → connects, ping ok
//   4. backend bound to 127.0.0.1 (no secret)                → connects (back-compat)
//
// Uses ws's per-message `Authorization` option on handshake, which exercises
// the same HTTP handshake path a browser / remote VS Code would take.
//
import { spawn } from 'node:child_process';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const exe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');

function pickPort() { return 40000 + Math.floor(Math.random() * 20000); }

function startBackend(args) {
    const p = spawn(exe, args, { stdio: ['ignore', 'pipe', 'pipe'] });
    p.stderr.on('data', d => process.stderr.write(`[be] ${d}`));
    return p;
}

async function wait(ms) { return new Promise(r => setTimeout(r, ms)); }

// Open a WS; resolve with {ok, code, err}. code = HTTP status on failure
// (401 for auth denial).
function tryConnect(url, headers) {
    return new Promise(res => {
        const ws = new WebSocket(url, { headers: headers || {} });
        let done = false;
        const finish = (v) => { if (!done) { done = true; try { ws.close(); } catch {} res(v); } };
        ws.on('open',     () => finish({ ok: true }));
        ws.on('unexpected-response', (_req, rsp) => finish({ ok: false, code: rsp.statusCode }));
        ws.on('error',    (e) => finish({ ok: false, err: e.message }));
        setTimeout(() => finish({ ok: false, err: 'timeout' }), 5000);
    });
}

// After a successful open, round-trip a ping to prove the session works.
function pingOnce(url, headers) {
    return new Promise((res, rej) => {
        const ws = new WebSocket(url, { headers: headers || {} });
        const id = Math.floor(Math.random() * 1e9);
        let got = false;
        ws.on('open',    () => ws.send(JSON.stringify({ type: 'cmd', id, name: 'ping' })));
        ws.on('message', (buf) => {
            const t = buf.toString();
            if (t[0] !== '{') return;
            try {
                const m = JSON.parse(t);
                if (m.type === 'rsp' && m.id === id && m.ok && m.data?.pong) {
                    got = true; ws.close();
                }
            } catch {}
        });
        ws.on('close',   () => got ? res(true) : rej(new Error('no pong')));
        ws.on('error',   (e) => rej(e));
        setTimeout(() => { if (!got) { try { ws.close(); } catch {}; rej(new Error('ping timeout')); } }, 5000);
    });
}

let failed = 0;
function check(label, cond, detail = '') {
    if (cond) console.log(`  ✓ ${label}`);
    else      { console.log(`  ✗ ${label}${detail ? ' — ' + detail : ''}`); failed++; }
}

// --- Case A: 0.0.0.0 + secret ---
const secret = 'hunter2-correct-horse-battery-staple';
const portA = pickPort();
console.log(`--- Case A: --host 0.0.0.0 --auth=<secret>, port=${portA} ---`);
const backendA = startBackend([`--port=${portA}`, '--host=0.0.0.0', `--auth=${secret}`]);
await wait(2500);

const urlA = `ws://127.0.0.1:${portA}`;
{
    const r = await tryConnect(urlA);
    check('no Authorization header → rejected', !r.ok, JSON.stringify(r));
    check('no Authorization header → 401 status', r.code === 401, `code=${r.code}`);
}
{
    const r = await tryConnect(urlA, { Authorization: 'Bearer wrong-secret' });
    check('wrong Bearer → rejected', !r.ok, JSON.stringify(r));
    check('wrong Bearer → 401 status', r.code === 401, `code=${r.code}`);
}
{
    const r = await tryConnect(urlA, { Authorization: `Bearer ${secret}` });
    check('correct Bearer → connects', r.ok, JSON.stringify(r));
}
try {
    await pingOnce(urlA, { Authorization: `Bearer ${secret}` });
    check('authenticated ping round-trips', true);
} catch (e) { check('authenticated ping round-trips', false, e.message); }

backendA.kill();
await wait(800);

// --- Case B: loopback-only, no secret (back-compat) ---
const portB = pickPort();
console.log(`\n--- Case B: default (loopback, no auth), port=${portB} ---`);
const backendB = startBackend([`--port=${portB}`]);
await wait(2500);

try {
    await pingOnce(`ws://127.0.0.1:${portB}`);
    check('loopback connect without any header → works', true);
} catch (e) { check('loopback connect without any header → works', false, e.message); }

backendB.kill();
await wait(800);

if (failed > 0) { console.error(`\nFAIL: ${failed} check(s) failed`); process.exit(1); }
console.log('\nOK: remote-auth handshake works as specified');
