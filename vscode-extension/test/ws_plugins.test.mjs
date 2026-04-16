// Plugin + project management test — discover plugins, create project,
// create instance, exchange commands with instance, save config.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { existsSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.textQueue = []; this.textWaiters = [];
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
    nextText(ms = 10000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('timeout')), ms);
            this.textWaiters.push({ resolve: v => { clearTimeout(t); resolve(v); } });
        });
    }
    async nextNonLog(ms = 10000) {
        for (;;) { const m = await this.nextText(ms); if (m.type !== 'log' && m.type !== 'event') return m; }
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

test('list_plugins discovers mock_camera and data_output', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello
        c.send({ type: 'cmd', id: 1, name: 'list_plugins' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        const plugins = rsp.data;
        assert.ok(Array.isArray(plugins));
        const names = plugins.map(p => p.name);
        console.log('plugins:', names.join(', '));
        assert.ok(names.includes('mock_camera'), 'mock_camera found');
        assert.ok(names.includes('data_output'), 'data_output found');
        const mc = plugins.find(p => p.name === 'mock_camera');
        assert.equal(mc.has_ui, true);
    });
});

test('create_project + create_instance + exchange + save', async () => {
    const projDir = resolve(tmpdir(), `xinsp2_test_proj_${Date.now()}`);

    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Load the mock_camera plugin
        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        const lr = await c.nextNonLog();
        assert.equal(lr.ok, true, 'load_plugin ok');

        // Create a project
        c.send({ type: 'cmd', id: 2, name: 'create_project', args: { folder: projDir, name: 'test_project' } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'create_project ok');
        assert.ok(existsSync(resolve(projDir, 'project.json')));
        assert.ok(existsSync(resolve(projDir, 'inspection.cpp')));

        // Create a camera instance
        c.send({ type: 'cmd', id: 3, name: 'create_instance', args: { name: 'cam0', plugin: 'mock_camera' } });
        const ir = await c.nextNonLog();
        assert.equal(ir.ok, true, 'create_instance ok');
        assert.ok(existsSync(resolve(projDir, 'instances', 'cam0', 'instance.json')));

        // Exchange: get status
        c.send({ type: 'cmd', id: 4, name: 'exchange_instance', args: { name: 'cam0', cmd: { command: 'get_status' } } });
        const er = await c.nextNonLog();
        assert.equal(er.ok, true, 'exchange ok');
        console.log('camera status:', JSON.stringify(er.data));

        // Exchange: set fps
        c.send({ type: 'cmd', id: 5, name: 'exchange_instance', args: { name: 'cam0', cmd: { command: 'set_fps', value: 15 } } });
        const fr = await c.nextNonLog();
        assert.equal(fr.ok, true, 'set_fps ok');

        // Save instance config
        c.send({ type: 'cmd', id: 6, name: 'save_instance_config', args: { name: 'cam0' } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true, 'save ok');

        // Verify saved config
        const saved = JSON.parse(readFileSync(resolve(projDir, 'instances', 'cam0', 'instance.json'), 'utf8'));
        assert.equal(saved.plugin, 'mock_camera');
        assert.ok(saved.config);
    });

    try { rmSync(projDir, { recursive: true }); } catch {}
});
