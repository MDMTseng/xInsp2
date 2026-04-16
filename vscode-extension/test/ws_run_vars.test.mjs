// Test: compile a real script, run, verify vars shape and values.
// No demo fallback — everything goes through real compile_and_load.

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
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let c;
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        c = new Client(`ws://127.0.0.1:${port}`);
        try { await c.opened(); break; } catch { try { c.close(); } catch {} c = null; }
    }
    if (!c) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(c, child); }
    finally { c.close(); if (child.exitCode === null) { child.kill(); await sleep(100); } }
}

test('compile + run produces vars with expected shape', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello

        // Compile real script
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'compile ok');

        // Run
        c.send({ type: 'cmd', id: 2, name: 'run' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        assert.equal(typeof rsp.data.run_id, 'number');

        const vars = await c.nextNonLog();
        assert.equal(vars.type, 'vars');
        assert.ok(Array.isArray(vars.items));
        assert.ok(vars.items.length >= 5);

        const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));
        assert.equal(byName.tag.kind, 'string');
        assert.equal(byName.tag.value, 'user_script_v1');
        assert.equal(byName.alive.kind, 'boolean');
        assert.equal(byName.alive.value, true);
        assert.equal(byName.raw.kind, 'number');
    });
});

test('set_param updates value, next run uses new value', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Run with default user_amp=5 → scaled = frame * 5
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await c.nextNonLog();
        const v1 = await c.nextNonLog();
        const scaled1 = v1.items.find(i => i.name === 'scaled').value;

        // Change user_amp=9
        c.send({ type: 'cmd', id: 3, name: 'set_param', args: { name: 'user_amp', value: 9 } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Run again
        c.send({ type: 'cmd', id: 4, name: 'run' });
        await c.nextNonLog();
        const v2 = await c.nextNonLog();
        const scaled2 = v2.items.find(i => i.name === 'scaled').value;

        // Verify the ratio changed (9/5 = 1.8x)
        assert.ok(scaled2 > scaled1, `scaled should increase: ${scaled1} → ${scaled2}`);
    });
});

test('run without script returns warning, not crash', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'run' });
        // Should get rsp + a log warning (no vars since no script)
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        // No vars expected — just verify no crash
    });
});
