// P1-4 — a failed mid-run compile_and_load keeps streaming the last-good DLL but
// must leave a persistent signal a status poll can see. The backend publishes a
// sticky "@compile" status component: text=="ok" after a good swap, a "degraded: …"
// string after a failed attempt. This rides the retained status map (re-pulled on
// every reconnect) so an unattended operator can detect the degraded line.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const goodScript = resolve(__dirname, '../../examples/record_demo.cpp');

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
    opened() {
        return new Promise((res, rej) => { this.ws.once('open', res); this.ws.once('error', rej); });
    }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    nextText(timeoutMs = 90000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('text timeout')), timeoutMs);
            this.textWaiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    // Wait for the rsp to a specific cmd id (skips log/event/vars interleaving).
    async rsp(id, timeoutMs = 90000) {
        for (;;) {
            const m = await this.nextText(timeoutMs);
            if (m.type === 'rsp' && m.id === id) return m;
        }
    }
    close() { try { this.ws.close(); } catch {} }
}

async function connect(port) {
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        const c = new Client(`ws://127.0.0.1:${port}`);
        try { await c.opened(); return c; } catch { try { c.close(); } catch {} }
    }
    throw new Error('connect failed');
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let client = await connect(port).catch((e) => { child.kill(); throw e; });
    try { return await fn(client, port); }
    finally {
        try { client.close(); } catch {}
        if (child.exitCode === null) { child.kill(); await sleep(100); }
    }
}

// Read cmd:status and return the "@compile" component's text (or undefined).
async function compileStatus(c, id) {
    c.send({ type: 'cmd', id, name: 'status' });
    const r = await c.rsp(id);
    assert.equal(r.ok, true, 'status ok');
    return r.data?.['@compile']?.text;
}

test('compile health marker: ok -> degraded -> recover', { timeout: 240000 }, async () => {
    // A deliberately-broken script (#error fails cl.exe immediately, no includes needed).
    const dir = mkdtempSync(join(tmpdir(), 'xinsp2-p14-'));
    const badScript = join(dir, 'broken.cpp');
    writeFileSync(badScript, '#error P1-4 deliberate compile failure\n');

    await withBackend(async (c, port) => {
        await c.nextText(); // hello

        // 1) Good compile → @compile == "ok".
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: goodScript } });
        const good = await c.rsp(1);
        assert.equal(good.ok, true, 'good compile_and_load ok (data: ' + JSON.stringify(good.data) + ')');
        assert.equal(await compileStatus(c, 2), 'ok', '@compile is ok after a good load');

        // 2) Broken compile → rsp ok:false, last-good still loaded, marker degraded.
        c.send({ type: 'cmd', id: 3, name: 'compile_and_load', args: { path: badScript } });
        const bad = await c.rsp(3);
        assert.equal(bad.ok, false, 'broken compile_and_load replies ok:false');
        const degraded = await compileStatus(c, 4);
        assert.ok(degraded && degraded.startsWith('degraded'),
            `@compile must be degraded after a failed compile, got: ${JSON.stringify(degraded)}`);

        // 2b) The marker survives a reconnect — it's in the retained status map, not
        // a one-shot event. Close this client and reconnect (single-client backend),
        // re-pull cmd:status, and the degraded state is still there.
        c.close();
        await sleep(200);
        const c2 = await connect(port);
        await c2.nextText(); // hello
        const afterReconnect = await compileStatus(c2, 1);
        assert.ok(afterReconnect && afterReconnect.startsWith('degraded'),
            `degraded marker must survive reconnect, got: ${JSON.stringify(afterReconnect)}`);

        // 3) Recompile the good script → marker clears back to ok.
        c2.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: goodScript } });
        const recover = await c2.rsp(2);
        assert.equal(recover.ok, true, 'recovery compile ok');
        assert.equal(await compileStatus(c2, 3), 'ok', '@compile clears to ok after recovery');
        c2.close();
    });
});
