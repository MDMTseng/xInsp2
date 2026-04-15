// M3 integration test — run demo inspection, verify vars message shape.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(
    __dirname,
    '../../backend/build/Release/xinsp-backend.exe'
);

function randomPort() {
    return 30000 + Math.floor(Math.random() * 20000);
}

class BufferedClient {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.queue = [];
        this.waiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) return;
            try {
                const obj = JSON.parse(data.toString());
                const w = this.waiters.shift();
                if (w) w.resolve(obj);
                else   this.queue.push(obj);
            } catch {}
        });
    }
    opened() {
        return new Promise((resolve, reject) => {
            this.ws.once('open', resolve);
            this.ws.once('error', reject);
        });
    }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    next(timeoutMs = 5000) {
        if (this.queue.length) return Promise.resolve(this.queue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('next timeout')), timeoutMs);
            this.waiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let client;
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        const c = new BufferedClient(`ws://127.0.0.1:${port}`);
        try { await c.opened(); client = c; break; } catch { try { c.close(); } catch {} }
    }
    if (!client) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(client, child); }
    finally {
        client.close();
        if (child.exitCode === null) { child.kill(); await sleep(50); }
    }
}

test('cmd run produces vars message with expected shape', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'run' });

        // First the rsp, then the vars broadcast.
        const rsp = await c.next();
        assert.equal(rsp.type, 'rsp');
        assert.equal(rsp.ok, true);
        assert.equal(typeof rsp.data.run_id, 'number');
        assert.equal(typeof rsp.data.ms, 'number');

        const vars = await c.next();
        assert.equal(vars.type, 'vars');
        assert.equal(vars.run_id, rsp.data.run_id);
        assert.ok(Array.isArray(vars.items));
        assert.ok(vars.items.length >= 7);  // gray, doubled, sq1, sq2, score, label, ok

        const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));

        assert.equal(byName.gray.kind, 'number');
        assert.equal(byName.gray.value, 7 * 10);           // frame * demo_amp default
        assert.equal(byName.doubled.kind, 'number');
        assert.equal(byName.doubled.value, 140);
        assert.equal(byName.sq1.kind, 'number');
        assert.equal(byName.sq1.value, 70 * 70);
        assert.equal(byName.sq2.kind, 'number');
        assert.equal(byName.sq2.value, 140 * 140);
        assert.equal(byName.label.kind, 'string');
        assert.equal(byName.label.value, 'demo');
        assert.equal(byName.ok.kind, 'boolean');
        assert.equal(byName.ok.value, true);
    });
});

test('set_param updates value, next run uses new value', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello

        // Baseline run with demo_amp=10
        c.send({ type: 'cmd', id: 1, name: 'run' });
        const _rsp1 = await c.next();
        const v1 = await c.next();
        const gray1 = v1.items.find(i => i.name === 'gray').value;
        assert.equal(gray1, 70);

        // Change param
        c.send({ type: 'cmd', id: 2, name: 'set_param', args: { name: 'demo_amp', value: 5 } });
        const setRsp = await c.next();
        assert.equal(setRsp.ok, true);

        // Rerun — gray should now be 7 * 5 = 35
        c.send({ type: 'cmd', id: 3, name: 'run' });
        const _rsp2 = await c.next();
        const v2 = await c.next();
        const gray2 = v2.items.find(i => i.name === 'gray').value;
        assert.equal(gray2, 35);
    });
});

test('list_params returns both demo params', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'list_params' });
        const rsp = await c.next();
        assert.equal(rsp.ok, true);
        const inst = await c.next();
        assert.equal(inst.type, 'instances');
        const byName = Object.fromEntries(inst.params.map(p => [p.name, p]));
        assert.ok(byName.demo_amp);
        assert.ok(byName.demo_bias);
        assert.equal(byName.demo_amp.type, 'int');
        assert.equal(byName.demo_bias.type, 'float');
    });
});
