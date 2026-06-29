// ws_lifecycle_resume.test.mjs — a lifecycle op that quiesces dispatch must RESUME
// continuous streaming when it completes (DispatchPoolGuard RAII). Regression for
// F2: recompile/rebuild/commit/discard/export quiesced the pool and never respawned
// it, so a hot-recompile in continuous mode silently stopped the live stream with no
// signal to the client. Here we drive commit_working_copy (a guard-RESUME path) and
// assert vars frames keep arriving after it.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { mkdtempSync, writeFileSync } from 'node:fs';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.q = []; this.w = [];
        this.ws.on('message', (d, bin) => {
            if (bin) return;
            try { const o = JSON.parse(d.toString()); const x = this.w.shift(); if (x) x.resolve(o); else this.q.push(o); } catch {}
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(o) { this.ws.send(JSON.stringify(o)); }
    next(ms = 90000) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('timeout')), ms); this.w.push({ resolve: v => { clearTimeout(t); res(v); } }); });
    }
    async nextRsp(ms = 90000) { for (;;) { const m = await this.next(ms); if (m.type === 'rsp') return m; } }
    // Count completed runs via the run_finished event (the vars frame was removed
    // with VAR; run_finished still brackets every inspect, so it's the non-vars
    // "a frame ran" signal).
    drainRuns() { const v = this.q.filter(m => m.type === 'event' && m.name === 'run_finished'); this.q = this.q.filter(m => !(m.type === 'event' && m.name === 'run_finished')); return v; }
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

const SCRIPT =
    '#include <xi/xi.hpp>\n' +
    'static int g_n = 0;\n' +
    'XI_SCRIPT_EXPORT void xi_inspect_entry(int){ VAR(tick, g_n++); }\n';

test('continuous streaming survives a working-copy commit (DispatchPoolGuard resume)', async () => {
    const proj = mkdtempSync(join(tmpdir(), 'xi_resume_'));
    writeFileSync(join(proj, 'inspect.cpp'), SCRIPT);
    writeFileSync(join(proj, 'project.json'), JSON.stringify({ name: 'resume_demo', script: 'inspect.cpp' }));

    await withBackend(async (c) => {
        await c.next(); // hello
        // Open in working-copy mode so commit_working_copy has something to commit.
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: proj, working_copy: true } });
        assert.equal((await c.nextRsp()).ok, true, 'open (working_copy) ok');

        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: join(proj, 'inspect.cpp') } });
        assert.equal((await c.nextRsp()).ok, true, 'compile ok');

        c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 20 } });
        assert.equal((await c.nextRsp()).ok, true, 'start ok');

        await sleep(800);
        assert.ok(c.drainRuns().length >= 2, 'streaming before the commit');

        // commit_working_copy quiesces the dispatch pool. Before the fix it never
        // resumed → the stream stopped here silently.
        c.send({ type: 'cmd', id: 4, name: 'commit_working_copy' });
        assert.equal((await c.nextRsp()).ok, true, 'commit_working_copy ok');

        // The stream must resume on its own. Drain stale runs, then confirm fresh ones.
        c.drainRuns();
        await sleep(800);
        assert.ok(c.drainRuns().length >= 2, 'streaming RESUMED after the commit (regression: it stopped)');

        c.send({ type: 'cmd', id: 5, name: 'stop' });
        assert.equal((await c.nextRsp()).ok, true, 'stop ok');
    });
});
