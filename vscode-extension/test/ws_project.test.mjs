// M6 integration test — instances, set_instance_def, save/load project.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { readFileSync, unlinkSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const scriptPath = resolve(__dirname, '../../examples/user_with_instance.cpp');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.textQueue = [];
        this.textWaiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) return;
            try {
                const obj = JSON.parse(data.toString());
                const w = this.textWaiters.shift();
                if (w) w.resolve(obj); else this.textQueue.push(obj);
            } catch {}
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    nextText(ms = 90000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('timeout')), ms);
            this.textWaiters.push({ resolve: v => { clearTimeout(t); resolve(v); } });
        });
    }
    async nextNonLog(ms = 90000) {
        for (;;) { const m = await this.nextText(ms); if (m.type !== 'log') return m; }
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

test('list_instances returns loaded script instances', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Compile
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'compile ok');

        // List instances
        c.send({ type: 'cmd', id: 2, name: 'list_instances' });
        const rsp = await c.nextNonLog(); // rsp ok
        assert.equal(rsp.ok, true);
        const inst = await c.nextNonLog(); // instances msg
        assert.equal(inst.type, 'instances');
        const scaler = inst.instances.find(i => i.name === 'my_scaler');
        assert.ok(scaler, 'my_scaler should exist');
        assert.equal(scaler.plugin, 'Scaler');
        assert.equal(scaler.def.factor, 2); // default
    });
});

test('set_instance_def changes behavior on next run', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Run with default factor=2: scaled = 7*10*2 = 140
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await c.nextNonLog(); // rsp
        const v1 = await c.nextNonLog();
        assert.equal(v1.items.find(i => i.name === 'scaled').value, 140);

        // Change factor to 5
        c.send({ type: 'cmd', id: 3, name: 'set_instance_def', args: { name: 'my_scaler', def: { factor: 5 } } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Run again: scaled = 7*10*5 = 350
        c.send({ type: 'cmd', id: 4, name: 'run' });
        await c.nextNonLog();
        const v2 = await c.nextNonLog();
        assert.equal(v2.items.find(i => i.name === 'scaled').value, 350);
    });
});

test('save_project + load_project round-trip', async () => {
    const projFile = resolve(tmpdir(), `xinsp2_test_${Date.now()}.json`);

    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Set param and instance def
        c.send({ type: 'cmd', id: 2, name: 'set_param', args: { name: 'base_val', value: 20 } });
        assert.equal((await c.nextNonLog()).ok, true);
        c.send({ type: 'cmd', id: 3, name: 'set_instance_def', args: { name: 'my_scaler', def: { factor: 7 } } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Save
        c.send({ type: 'cmd', id: 4, name: 'save_project', args: { path: projFile } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Verify file exists
        assert.ok(existsSync(projFile), 'project.json written');
        const saved = JSON.parse(readFileSync(projFile, 'utf8'));
        assert.ok(Array.isArray(saved.params));
        assert.ok(Array.isArray(saved.instances));
        const bp = saved.params.find(p => p.name === 'base_val');
        assert.ok(bp, 'base_val in saved params');
        assert.equal(bp.value, 20);
    });

    // New backend, reload project, verify param is restored.
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Load project
        c.send({ type: 'cmd', id: 2, name: 'load_project', args: { path: projFile } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Run — base_val should be 20 (restored): input = 7*20 = 140
        c.send({ type: 'cmd', id: 3, name: 'run' });
        await c.nextNonLog();
        const vars = await c.nextNonLog();
        assert.equal(vars.items.find(i => i.name === 'input').value, 140);
    });

    try { unlinkSync(projFile); } catch {}
});
