// Test: compile a script, start continuous mode, verify multiple runs.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const scriptPath = resolve(__dirname, '../../examples/user_script_example.cpp');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.queue = []; this.waiters = [];
        this.ws.on('message', (d, bin) => {
            if (bin) return;
            try { const o = JSON.parse(d.toString()); const w = this.waiters.shift(); if (w) w.resolve(o); else this.queue.push(o); } catch {}
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(o) { this.ws.send(JSON.stringify(o)); }
    next(ms = 90000) {
        if (this.queue.length) return Promise.resolve(this.queue.shift());
        return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('timeout')), ms); this.waiters.push({ resolve: v => { clearTimeout(t); res(v); } }); });
    }
    async nextNonLog(ms = 90000) { for (;;) { const m = await this.next(ms); if (m.type !== 'log') return m; } }
    drainText() { const all = [...this.queue]; this.queue = []; return all; }
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let c;
    for (let i = 0; i < 30; ++i) {
        await sleep(100); c = new Client(`ws://127.0.0.1:${port}`);
        try { await c.opened(); break; } catch { try { c.close(); } catch {} c = null; }
    }
    if (!c) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(c, child); }
    finally { c.close(); if (child.exitCode === null) { child.kill(); await sleep(100); } }
}

test('compile + start continuous triggers multiple inspections', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello

        // Compile first
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'compile ok');

        // Start continuous at 10fps
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 10 } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);

        // Wait for runs
        await sleep(1500);
        const msgs = c.drainText();
        const varsMsgs = msgs.filter(m => m.type === 'vars');
        console.log(`received ${varsMsgs.length} vars messages`);
        assert.ok(varsMsgs.length >= 2, `expected >=2 vars, got ${varsMsgs.length}`);

        // Stop
        c.send({ type: 'cmd', id: 3, name: 'stop' });
        const stopRsp = await c.nextNonLog();
        assert.equal(stopRsp.ok, true);

        await sleep(500);
        const after = c.drainText().filter(m => m.type === 'vars');
        assert.equal(after.length, 0, 'no vars after stop');
    });
});
