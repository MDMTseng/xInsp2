// Test: compile defect_detection.cpp, run, verify vars shape.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const scriptPath = resolve(__dirname, '../../examples/defect_detection.cpp');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.textQueue = []; this.textWaiters = []; this.binaryCount = 0;
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) { this.binaryCount++; return; }
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

test('defect_detection.cpp compiles, runs, produces expected vars', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'compile ok: ' + (cr.error ?? ''));

        c.send({ type: 'cmd', id: 2, name: 'run' });
        const rr = await c.nextNonLog(); // rsp
        assert.equal(rr.ok, true);

        const vars = await c.nextNonLog(); // vars
        assert.equal(vars.type, 'vars');
        const names = vars.items.map(i => i.name);
        console.log('vars:', names.join(', '));

        assert.ok(names.includes('input'),          'has input');
        assert.ok(names.includes('gray'),           'has gray');
        assert.ok(names.includes('blurred'),        'has blurred');
        assert.ok(names.includes('binary'),         'has binary');
        assert.ok(names.includes('cleaned'),        'has cleaned');
        assert.ok(names.includes('edges'),          'has edges');
        assert.ok(names.includes('blob_count'),     'has blob_count');
        assert.ok(names.includes('mean_intensity'), 'has mean_intensity');
        assert.ok(names.includes('pass'),           'has pass');

        // blob_count should be a number
        const bc = vars.items.find(i => i.name === 'blob_count');
        assert.equal(bc.kind, 'number');
        assert.ok(bc.value >= 0);

        // pass should be boolean
        const ps = vars.items.find(i => i.name === 'pass');
        assert.equal(ps.kind, 'boolean');

        // Images should have gid
        const imgVars = vars.items.filter(i => i.kind === 'image');
        assert.ok(imgVars.length >= 5, `expected >=5 image vars, got ${imgVars.length}`);

        // Wait for binary frames
        await sleep(1000);
        console.log(`received ${c.binaryCount} binary preview frames`);
        assert.ok(c.binaryCount >= 3, `expected >=3 previews, got ${c.binaryCount}`);
    });
});
