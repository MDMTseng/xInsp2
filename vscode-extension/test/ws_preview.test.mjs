// Test: compile defect_detection, run, verify JPEG preview binary frame.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import { writeFileSync, mkdtempSync } from 'node:fs';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const scriptPath = resolve(__dirname, '../../examples/defect_detection.cpp');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.textQueue = []; this.textWaiters = [];
        this.binaryQueue = []; this.binaryWaiters = [];
        this.ws.on('message', (d, bin) => {
            if (bin) { const w = this.binaryWaiters.shift(); if (w) w.resolve(d); else this.binaryQueue.push(d); }
            else { try { const o = JSON.parse(d.toString()); const w = this.textWaiters.shift(); if (w) w.resolve(o); else this.textQueue.push(o); } catch {} }
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(o) { this.ws.send(JSON.stringify(o)); }
    nextText(ms = 90000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('timeout')), ms); this.textWaiters.push({ resolve: v => { clearTimeout(t); res(v); } }); });
    }
    async nextNonLog(ms = 90000) { for (;;) { const m = await this.nextText(ms); if (m.type !== 'log' && m.type !== 'event') return m; } }
    nextBinary(ms = 5000) {
        if (this.binaryQueue.length) return Promise.resolve(this.binaryQueue.shift());
        return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('bin timeout')), ms); this.binaryWaiters.push({ resolve: v => { clearTimeout(t); res(v); } }); });
    }
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
    try { return await fn(c, child); }
    finally { c.close(); if (child.exitCode === null) { child.kill(); await sleep(100); } }
}

test('compile + run emits JPEG preview for image variables', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'compile ok');

        c.send({ type: 'cmd', id: 2, name: 'run' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);

        const vars = await c.nextNonLog();
        assert.equal(vars.type, 'vars');
        const imgs = vars.items.filter(i => i.kind === 'image');
        assert.ok(imgs.length >= 3, `expected >=3 image vars, got ${imgs.length}`);

        // Should receive at least one binary preview frame
        const frame = await c.nextBinary();
        assert.ok(frame.length > 20, 'preview frame too short');
        // JPEG magic
        const jpg = frame.subarray(20);
        assert.equal(jpg[0], 0xFF);
        assert.equal(jpg[1], 0xD8);
    });
});

// When one image buffer is VAR'd by several names (the "one frame to every
// plugin" case), every var shares a canonical "src" gid, and the backend
// encodes + sends exactly one preview frame for the whole group.
test('same buffer VAR\'d 3x → shared src + a single preview frame', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_dedup_'));
    const sp = join(dir, 'dedup.cpp');
    writeFileSync(sp, `
#include <xi/xi.hpp>
#include <xi/xi_image.hpp>
#include <cstring>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::Image a(64, 48, 1);
    std::memset(a.data(), 128, 64 * 48);
    VAR(orig, a);      // first sighting → canonical
    VAR(same, orig);   // copy shares a's buffer
    VAR(also, a);      // also a's buffer
}
`);
    await withBackend(async (c) => {
        await c.nextText(); // hello
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: sp } });
        assert.equal((await c.nextNonLog()).ok, true, 'compile ok');

        c.send({ type: 'cmd', id: 2, name: 'run' });
        assert.equal((await c.nextNonLog()).ok, true);

        const vars = await c.nextNonLog();
        assert.equal(vars.type, 'vars');
        const imgs = vars.items.filter(i => i.kind === 'image');
        assert.equal(imgs.length, 3, `expected 3 image vars, got ${imgs.length}`);

        // All three share one canonical src (== the first var's gid).
        const srcs = new Set(imgs.map(i => i.src));
        assert.equal(srcs.size, 1, `expected 1 canonical src, got ${[...srcs]}`);
        assert.equal(imgs[0].src, imgs[0].gid, 'first var is its own canon');
        assert.ok(imgs.every(i => i.gid !== undefined && i.src !== undefined), 'gid+src present');

        // Exactly one preview frame for the whole canon group.
        let frames = 0;
        for (;;) {
            try { await c.nextBinary(2500); frames++; }
            catch { break; }
        }
        assert.equal(frames, 1, `expected 1 deduped preview frame, got ${frames}`);
    });
});
