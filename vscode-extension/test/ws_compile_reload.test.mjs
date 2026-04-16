// M5 integration test — compile_and_load a user script over WS, run it,
// verify the vars it produced, tune a param, rerun, verify new values.

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
        this.textQueue = [];
        this.binaryQueue = [];
        this.textWaiters = [];
        this.binaryWaiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) {
                const w = this.binaryWaiters.shift();
                if (w) w.resolve(data); else this.binaryQueue.push(data);
            } else {
                try {
                    const obj = JSON.parse(data.toString());
                    const w = this.textWaiters.shift();
                    if (w) w.resolve(obj); else this.textQueue.push(obj);
                } catch {}
            }
        });
    }
    opened() {
        return new Promise((res, rej) => {
            this.ws.once('open', res);
            this.ws.once('error', rej);
        });
    }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    nextText(timeoutMs = 90000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('text timeout')), timeoutMs);
            this.textWaiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    async nextNonLogText(timeoutMs = 90000) {
        // Skip log messages (from build output etc.)
        for (;;) {
            const m = await this.nextText(timeoutMs);
            if (m.type === 'log') continue;
            return m;
        }
    }
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let client;
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        const c = new Client(`ws://127.0.0.1:${port}`);
        try { await c.opened(); client = c; break; } catch { try { c.close(); } catch {} }
    }
    if (!client) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(client, child); }
    finally {
        client.close();
        if (child.exitCode === null) { child.kill(); await sleep(100); }
    }
}

test('compile_and_load + run end-to-end', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Trigger compile
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const rsp = await c.nextNonLogText();
        assert.equal(rsp.type, 'rsp');
        if (!rsp.ok) {
            // Grab any follow-up log (build_log is sent as a `log` msg on failure)
            throw new Error('compile_and_load failed: ' + (rsp.error || 'unknown'));
        }
        assert.ok(rsp.data.dll);

        // Run the loaded script
        c.send({ type: 'cmd', id: 2, name: 'run' });
        const runRsp = await c.nextNonLogText();
        assert.equal(runRsp.ok, true);

        const vars = await c.nextNonLogText();
        assert.equal(vars.type, 'vars');
        const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));

        // frame=1, user_amp=5 → raw=1, scaled=5, dbl=10
        assert.equal(byName.raw.value, 1);
        assert.equal(byName.scaled.value, 5);
        assert.equal(byName.dbl.value, 10);
        assert.equal(byName.tag.value, 'user_script_v1');
        assert.equal(byName.alive.value, true);
    });
});

test('set_param on loaded script updates next run', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const rsp = await c.nextNonLogText();
        assert.equal(rsp.ok, true, 'compile ok');

        // First run with default user_amp = 5
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await c.nextNonLogText();             // rsp
        const v1 = await c.nextNonLogText();  // vars
        assert.equal(v1.items.find(i => i.name === 'scaled').value, 5);

        // Change user_amp = 9
        c.send({ type: 'cmd', id: 3, name: 'set_param', args: { name: 'user_amp', value: 9 } });
        const sr = await c.nextNonLogText();
        assert.equal(sr.ok, true);

        // Re-run: scaled = 1 * 9 = 9
        c.send({ type: 'cmd', id: 4, name: 'run' });
        await c.nextNonLogText();             // rsp
        const v2 = await c.nextNonLogText();  // vars
        assert.equal(v2.items.find(i => i.name === 'scaled').value, 9);
    });
});
