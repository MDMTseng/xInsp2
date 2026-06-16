// ws_fallback_gate.test.mjs — γ-4 load-time gate: a plugin whose yyjson layout
// doesn't match the host (here: a hand-coded plugin with NO xi_yyjson_abi export)
// can only run the slow JSON path, so it is REFUSED at load — unless its
// plugin.json opts in with "json_fallback": true.
//
// Fixture: mock_camera.dll is hand-coded (no XI_PLUGIN_IMPL → no xi_yyjson_abi),
// so it's a ready-made "incompatible" DLL. We drop a copy into two temp plugin
// folders (one without the opt-in, one with) and load each via --plugins-dir.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mkdirSync, copyFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe  = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const noExportDll = resolve(__dirname, '../../plugins/mock_camera/mock_camera.dll');

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

async function withBackend(extraArgs, fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`, ...extraArgs],
                        { stdio: ['ignore', 'inherit', 'inherit'] });
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

function makePlugin(dir, name, optIn) {
    const folder = join(dir, name);
    mkdirSync(folder, { recursive: true });
    copyFileSync(noExportDll, join(folder, `${name}.dll`));
    const manifest = { name, description: 'gate test fixture',
                       dll: `${name}.dll`, factory: 'xi_plugin_create' };
    if (optIn) manifest.json_fallback = true;
    writeFileSync(join(folder, 'plugin.json'), JSON.stringify(manifest, null, 2));
}

test('layout-mismatch plugin refused at load; json_fallback opt-in loads it', async () => {
    const base = resolve(tmpdir(), `xinsp2_gate_${Date.now()}`);
    makePlugin(base, 'gate_noopt', false);
    makePlugin(base, 'gate_opt',   true);
    try {
        await withBackend([`--plugins-dir=${base}`], async (c) => {
            await c.nextText(); // hello

            // No opt-in: the no-xi_yyjson_abi DLL can only run the JSON path, so
            // the gate REFUSES it at load.
            c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'gate_noopt' } });
            const r1 = await c.nextNonLog();
            assert.equal(r1.ok, false, 'gate_noopt refused (no xi_yyjson_abi export, no opt-in)');

            // Opt-in: same DLL, but plugin.json sets json_fallback → it loads on
            // the JSON path.
            c.send({ type: 'cmd', id: 2, name: 'load_plugin', args: { name: 'gate_opt' } });
            const r2 = await c.nextNonLog();
            assert.equal(r2.ok, true, 'gate_opt loads via json_fallback opt-in');
        });
    } finally {
        try { rmSync(base, { recursive: true }); } catch {}
    }
});
