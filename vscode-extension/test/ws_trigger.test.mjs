// Trigger loop test — start continuous mode, receive multiple vars updates,
// then stop.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.textQueue = [];
        this.binaryQueue = [];
        this.textWaiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) {
                this.binaryQueue.push(data);
            } else {
                try {
                    const obj = JSON.parse(data.toString());
                    const w = this.textWaiters.shift();
                    if (w) w.resolve(obj);
                    else   this.textQueue.push(obj);
                } catch {}
            }
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    nextText(ms = 10000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('timeout')), ms);
            this.textWaiters.push({ resolve: v => { clearTimeout(t); resolve(v); } });
        });
    }
    async nextNonLog(ms = 10000) {
        for (;;) { const m = await this.nextText(ms); if (m.type !== 'log') return m; }
    }
    drainText() {
        const all = [...this.textQueue];
        this.textQueue = [];
        return all;
    }
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

test('start creates default TestImageSource and triggers inspections', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Start continuous
        c.send({ type: 'cmd', id: 1, name: 'start' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);

        // Wait for a few triggered runs (TestImageSource at 5fps = one every 200ms)
        await sleep(1500);

        // Drain all received messages — should have multiple vars and events
        const msgs = c.drainText();
        const varsMsgs = msgs.filter(m => m.type === 'vars');
        const events = msgs.filter(m => m.type === 'event' && m.name === 'run_finished');

        console.log(`received ${varsMsgs.length} vars messages, ${events.length} run_finished events`);
        assert.ok(varsMsgs.length >= 2, `expected >=2 vars, got ${varsMsgs.length}`);
        assert.ok(events.length >= 2, `expected >=2 events, got ${events.length}`);

        // Each vars should have the demo's tracked variables
        for (const v of varsMsgs) {
            assert.ok(v.items.length >= 7, 'expected demo vars');
        }

        // Stop
        c.send({ type: 'cmd', id: 2, name: 'stop' });
        const stopRsp = await c.nextNonLog();
        assert.equal(stopRsp.ok, true);

        // No more runs after stop — drain after a short wait
        await sleep(500);
        const after = c.drainText().filter(m => m.type === 'vars');
        assert.equal(after.length, 0, 'no vars after stop');
    });
});
