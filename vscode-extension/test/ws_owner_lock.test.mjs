// F5 — opening a project rebased writes to a working copy by convention only;
// nothing warned if a SECOND backend (or the same project opened elsewhere) was
// already writing the same canonical, and a later working-copy commit would clobber
// it. Each open now drops an advisory .xinsp_owner stamp (pid + ts) at the canonical
// root; opening a project a LIVE different process already stamped surfaces a warn.
// Advisory only — a stale stamp (dead pid) is silently taken over.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeFileSync, mkdtempSync, existsSync } from 'node:fs';
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
    next(timeoutMs = 60000) {
        if (this.q.length) return Promise.resolve(this.q.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('text timeout')), timeoutMs);
            this.waiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    // Read until the rsp for `id`; return { rsp, logs:[warn/error msgs seen before it] }.
    async openResult(id) {
        const logs = [];
        for (;;) {
            const m = await this.next();
            if (m.type === 'log') logs.push(m);
            if (m.type === 'rsp' && m.id === id) return { rsp: m, logs };
        }
    }
    close() { try { this.ws.close(); } catch {} }
}

async function spawnBackend() {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        const c = new Client(`ws://127.0.0.1:${port}`);
        try { await c.opened(); await c.next(); /* hello */ return { c, child }; }
        catch { try { c.close(); } catch {} }
    }
    child.kill(); throw new Error('connect failed');
}

test('F5: a second live backend opening the same project is warned', { timeout: 120000 }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xinsp2-f5-'));
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'f5_lock', script: 'inspect.cpp', params: [], instances: [] }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'),
        '#include <xi/xi.hpp>\nXI_SCRIPT_EXPORT\nvoid xi_inspect_entry(int frame) {}\n');

    const A = await spawnBackend();
    const B = await spawnBackend();
    try {
        // Backend A opens first → stamps .xinsp_owner with A's pid, no prior owner → no warn.
        A.c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        const aRes = await A.c.openResult(1);
        assert.equal(aRes.rsp.ok, true, 'A open ok');
        assert.ok(!aRes.logs.some(l => /another backend/.test(l.msg || '')),
            'A (first opener) must NOT warn');
        assert.ok(existsSync(join(dir, '.xinsp_owner')), '.xinsp_owner stamp written');

        // Backend B (alive A still holds the stamp) opens the same folder → warned.
        B.c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        const bRes = await B.c.openResult(1);
        assert.equal(bRes.rsp.ok, true, 'B open still succeeds (advisory, never refuses)');
        const warned = bRes.logs.some(l => l.level === 'warn' && /another backend \(pid \d+/.test(l.msg || ''));
        assert.ok(warned, 'B must be warned that another live backend owns the project: '
            + JSON.stringify(bRes.logs.map(l => l.msg)));
    } finally {
        A.c.close(); B.c.close();
        for (const h of [A, B]) if (h.child.exitCode === null) { h.child.kill(); }
        await sleep(150);
    }
});
