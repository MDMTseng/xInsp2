// P1-8 — the per-run dispatch counters (dropped, high_watermark) reset on every
// cmd:start (lanes are recreated), so a restart erased the "how much did we drop"
// history. dispatch_stats now also reports process-uptime cumulatives that do NOT
// reset: dropped_lifetime, queue_depth_high_watermark_lifetime.
//
// This induces real drops: queue_depth=1 + dispatch_threads=1 + a script that
// sleeps 80ms, fed by the 100fps synthetic timer (a tick every 10ms) → the queue
// overflows and drops. Then stop+start and confirm the per-run counter reset but
// the lifetime counter did not.

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
const slash = (p) => p.replace(/\\/g, '/');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url); this.q = []; this.waiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) return;
            try { const o = JSON.parse(data.toString()); const w = this.waiters.shift(); if (w) w.resolve(o); else this.q.push(o); } catch {}
        });
    }
    opened() { return new Promise((res, rej) => { this.ws.once('open', res); this.ws.once('error', rej); }); }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    next(timeoutMs = 90000) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('text timeout')), timeoutMs);
            this.waiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    async rsp(id, timeoutMs = 150000) { for (;;) { const m = await this.next(timeoutMs); if (m.type === 'rsp' && m.id === id) return m; } }
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
    try { return await fn(client); }
    finally { try { client.close(); } catch {} if (child.exitCode === null) { child.kill(); await sleep(100); } }
}

async function dispatchStats(c, id) {
    c.send({ type: 'cmd', id, name: 'dispatch_stats' });
    const r = await c.rsp(id);
    assert.equal(r.ok, true, 'dispatch_stats ok');
    return r.data;
}

test('dispatch dropped/high-watermark lifetime counters survive cmd:start (P1-8)', { timeout: 180000 }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xinsp2-p18-'));
    // queue_depth 1 + 1 worker + an 80ms inspect, fed by a 100fps (10ms) timer → drops.
    writeFileSync(join(dir, 'project.json'), JSON.stringify({
        name: 'p18_drops', script: 'inspect.cpp', params: [], instances: [],
        parallelism: { dispatch_threads: 1, queue_depth: 1, overflow: 'drop_oldest' },
    }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'),
        '#include <xi/xi.hpp>\n#include <thread>\n#include <chrono>\n' +
        'XI_SCRIPT_EXPORT\nvoid xi_inspect_entry(int frame) {\n' +
        '    std::this_thread::sleep_for(std::chrono::milliseconds(80));\n}\n');

    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        assert.equal((await c.rsp(1)).ok, true, 'open_project ok');
        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
        assert.equal((await c.rsp(2)).ok, true, 'compile_and_load ok');

        // Run continuous at 100fps for ~1.5s so the slow inspect overflows the depth-1 queue.
        c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 100 } });
        assert.equal((await c.rsp(3)).ok, true, 'start ok');
        await sleep(1500);

        const ds1 = await dispatchStats(c, 4);
        assert.ok(ds1.dropped > 0, `expected per-run drops, got ${ds1.dropped}`);
        assert.ok(ds1.dropped_lifetime > 0, `expected dropped_lifetime > 0, got ${ds1.dropped_lifetime}`);
        assert.ok(ds1.queue_depth_high_watermark_lifetime >= 1,
            `expected hw_lifetime >= 1, got ${ds1.queue_depth_high_watermark_lifetime}`);
        const lifeDrops = ds1.dropped_lifetime;
        const lifeHw = ds1.queue_depth_high_watermark_lifetime;

        // Restart: per-run counters reset (lanes recreated), lifetime must NOT.
        c.send({ type: 'cmd', id: 5, name: 'stop' });
        await c.rsp(5);
        c.send({ type: 'cmd', id: 6, name: 'start', args: { fps: 100 } });
        assert.equal((await c.rsp(6)).ok, true, 'restart ok');

        const ds2 = await dispatchStats(c, 7);   // immediately after restart
        assert.ok(ds2.dropped < lifeDrops,
            `per-run dropped should reset on start (was ${lifeDrops}, now ${ds2.dropped})`);
        assert.ok(ds2.dropped_lifetime >= lifeDrops,
            `dropped_lifetime must survive restart (was ${lifeDrops}, now ${ds2.dropped_lifetime})`);
        assert.ok(ds2.queue_depth_high_watermark_lifetime >= lifeHw,
            `hw_lifetime must survive restart (was ${lifeHw}, now ${ds2.queue_depth_high_watermark_lifetime})`);

        c.send({ type: 'cmd', id: 8, name: 'stop' });
        await c.rsp(8);
    });
});
