// Ordered output sink — a plugin marked "sink": true receives a script's
// use(<sink>).process(rec) in FRAME (arrival) order even under parallel dispatch,
// because the host stages the call and flushes it inside the ordered-emit gate.
//
// This builds the sdk/examples/comm plugin as a project source plugin, runs a script
// that (a) sleeps a scrambled amount so inspects FINISH out of order under
// dispatch_threads=4, and (b) calls use("comm0").process(...) each frame. The comm
// sink records the host-stamped $seq it receives, in delivery order. With the ordered
// sink the received $seq must be STRICTLY INCREASING (arrival order) — not scrambled.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeFileSync, mkdtempSync, mkdirSync, copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const commDir = resolve(__dirname, '../../sdk/examples/comm');
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
    next(timeoutMs = 150000) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('text timeout')), timeoutMs);
            this.waiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    async rsp(id) { for (;;) { const m = await this.next(); if (m.type === 'rsp' && m.id === id) return m; } }
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

test('ordered sink: use(sink).process() delivered in frame order under N>1', { timeout: 240000 }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xinsp2-sink-'));
    // 4 workers, arrival-ordered emission, a deep queue so we don't drop samples.
    writeFileSync(join(dir, 'project.json'), JSON.stringify({
        name: 'ordered_sink', script: 'inspect.cpp', params: [], instances: [],
        plugins: { comm: { path: 'comm', compile: true } },   // backend compiles plugins/comm/
        parallelism: { dispatch_threads: 4, queue_depth: 1000, overflow: 'drop_oldest', result_order: 'arrival' },
    }, null, 2));
    // Scrambled sleep → inspects finish out of completion order; the gate must
    // re-serialize the sink delivery into arrival order.
    writeFileSync(join(dir, 'inspect.cpp'),
        '#include <xi/xi.hpp>\n#include <xi/xi_use.hpp>\n#include <xi/xi_record.hpp>\n' +
        '#include <thread>\n#include <chrono>\n' +
        'XI_SCRIPT_EXPORT\nvoid xi_inspect_entry(int frame) {\n' +
        '    int ms = 4 + ((frame * 7) % 11) * 3;   // 4..34ms, non-monotonic\n' +
        '    std::this_thread::sleep_for(std::chrono::milliseconds(ms));\n' +
        '    xi::use("comm0").process(xi::Record().set("frame", frame));\n}\n');
    // The comm sink as a project source plugin (the backend compiles plugins/comm/).
    mkdirSync(join(dir, 'plugins', 'comm'), { recursive: true });
    copyFileSync(join(commDir, 'plugin.json'), join(dir, 'plugins', 'comm', 'plugin.json'));
    copyFileSync(join(commDir, 'comm.cpp'),    join(dir, 'plugins', 'comm', 'comm.cpp'));
    // comm0 instance.
    mkdirSync(join(dir, 'instances', 'comm0'), { recursive: true });
    writeFileSync(join(dir, 'instances', 'comm0', 'instance.json'), JSON.stringify({ plugin: 'comm' }, null, 2));

    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        assert.equal((await c.rsp(1)).ok, true, 'open_project ok');
        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
        const comp = await c.rsp(2);
        assert.equal(comp.ok, true, 'compile_and_load ok (data: ' + JSON.stringify(comp.data) + ')');

        c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 60 } });
        assert.equal((await c.rsp(3)).ok, true, 'start ok');
        await sleep(2000);
        c.send({ type: 'cmd', id: 4, name: 'stop' });
        await c.rsp(4);
        await sleep(200);   // let the last gated flush land

        c.send({ type: 'cmd', id: 5, name: 'exchange_instance', args: { name: 'comm0', cmd: { command: 'get_received' } } });
        const ex = await c.rsp(5);
        assert.equal(ex.ok, true, 'exchange ok');
        const data = typeof ex.data === 'string' ? JSON.parse(ex.data) : ex.data;
        const seqs = data.received || [];
        assert.ok(seqs.length >= 10, `expected a healthy sample of deliveries, got ${seqs.length}`);

        // The whole point: deliveries are in frame (arrival) order — strictly
        // increasing $seq — despite the scrambled completion order.
        for (let i = 1; i < seqs.length; ++i) {
            assert.ok(seqs[i] > seqs[i - 1],
                `sink received out of frame order at #${i}: seq ${seqs[i]} after ${seqs[i - 1]} ` +
                `(full: ${JSON.stringify(seqs.slice(0, 40))}${seqs.length > 40 ? '…' : ''})`);
        }
    });
});
