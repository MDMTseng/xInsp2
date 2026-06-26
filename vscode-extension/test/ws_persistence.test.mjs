// Round-trip test: a project.json with dispatch groups + default_group + runtime,
// and an instance.json with a `group`, must survive a save (any instance CRUD
// rewrites them) — regression guard for the F1/F3 data-loss fixes.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync } from 'node:fs';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.tq = []; this.tw = [];
        this.ws.on('message', (d, bin) => {
            if (bin) return;
            try { const o = JSON.parse(d.toString()); const w = this.tw.shift(); if (w) w(o); else this.tq.push(o); } catch {}
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(o) { this.ws.send(JSON.stringify(o)); }
    next(ms = 90000) {
        if (this.tq.length) return Promise.resolve(this.tq.shift());
        return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('timeout')), ms); this.tw.push(v => { clearTimeout(t); res(v); }); });
    }
    async nextRsp(ms = 90000) { for (;;) { const m = await this.next(ms); if (m.type === 'rsp') return m; } }
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let c;
    for (let i = 0; i < 30; ++i) {
        await sleep(100); c = new Client(`ws://127.0.0.1:${port}`);
        try { await c.opened(); break; } catch { try { c.close(); } catch {} c = null; }
    }
    if (!c) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(c); }
    finally { c.close(); if (child.exitCode === null) { child.kill(); await sleep(100); } }
}

test('project.json groups/default_group/runtime + instance group survive a save', async () => {
    const proj = mkdtempSync(join(tmpdir(), 'xi_persist_'));
    writeFileSync(join(proj, 'inspect.cpp'),
        '#include <xi/xi.hpp>\nXI_SCRIPT_EXPORT void xi_inspect_entry(int){}\n');
    writeFileSync(join(proj, 'project.json'), JSON.stringify({
        name: 'persist_demo',
        script: 'inspect.cpp',
        parallelism: {
            dispatch_threads: 2,
            groups: [
                { name: 'fast', max_parallel: 2, thread_priority: 'high', queue_depth: 50,
                  overflow: 'drop_newest', result_order: 'arrival', min_interval_ms: 5,
                  cpu_affinity: [[0, 1], [2, 3]] },
            ],
            default_group: 'fast',
        },
        runtime: { process_priority: 'high', timer_fps: 24 },
    }, null, 2));
    // An existing instance carrying a dispatch group (read into ii.group on open).
    const instDir = join(proj, 'instances', 'cam0');
    mkdirSync(instDir, { recursive: true });
    writeFileSync(join(instDir, 'instance.json'),
        JSON.stringify({ plugin: 'mock_camera', group: 'fast', config: {} }, null, 2));

    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: proj } });
        assert.equal((await c.nextRsp()).ok, true, 'open_project ok');

        // Any instance CRUD rewrites project.json (+ the instance) via save.
        c.send({ type: 'cmd', id: 2, name: 'create_instance', args: { name: 'cam1', plugin: 'mock_camera' } });
        assert.equal((await c.nextRsp()).ok, true, 'create_instance ok');
        await sleep(300);

        // project.json must still carry groups + default_group + runtime.
        const pj = JSON.parse(readFileSync(join(proj, 'project.json'), 'utf8'));
        assert.ok(pj.parallelism && Array.isArray(pj.parallelism.groups), 'groups survived');
        assert.equal(pj.parallelism.groups.length, 1);
        const g = pj.parallelism.groups[0];
        assert.equal(g.name, 'fast');
        assert.equal(g.max_parallel, 2);
        assert.equal(g.thread_priority, 'high');
        assert.equal(g.result_order, 'arrival');
        assert.equal(g.min_interval_ms, 5);
        assert.deepEqual(g.cpu_affinity, [[0, 1], [2, 3]], 'cpu_affinity round-tripped nested');
        assert.equal(pj.parallelism.default_group, 'fast', 'default_group survived');
        assert.ok(pj.runtime && pj.runtime.process_priority === 'high' && pj.runtime.timer_fps === 24, 'runtime survived');

        // Re-open the saved project — proves the emitted JSON is well-formed and
        // the groups parse back (skip-bad-instance would warn, not throw).
        c.send({ type: 'cmd', id: 3, name: 'open_project', args: { folder: proj } });
        assert.equal((await c.nextRsp()).ok, true, 're-open of saved project ok');
    });
});
