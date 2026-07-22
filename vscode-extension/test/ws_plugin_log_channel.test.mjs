// P1-3 — a plugin/script host_api->log(WARN|ERROR) used to reach only stderr,
// which is unwatched on an unattended PC. The backend now forwards WARN/ERROR to
// the operator channel: a live WS `log` message, and (for ERROR) the recent-errors
// ring surfaced by cmd:recent_errors.
//
// Trigger: a script that calls xi::use("<missing>") — the host logs an ERROR
// ("no such instance") through the same host_api->log path a plugin uses.

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

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.q = [];
        this.waiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) return;
            try {
                const obj = JSON.parse(data.toString());
                const w = this.waiters.shift();
                if (w) w.resolve(obj); else this.q.push(obj);
            } catch {}
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
    async rsp(id, timeoutMs = 90000) {
        for (;;) { const m = await this.next(timeoutMs); if (m.type === 'rsp' && m.id === id) return m; }
    }
    // Read until a message satisfies pred (or timeout). Returns the matching msg.
    async until(pred, timeoutMs = 30000) {
        const deadline = Date.now() + timeoutMs;
        for (;;) {
            const left = deadline - Date.now();
            if (left <= 0) throw new Error('until: timed out');
            const m = await this.next(left);
            if (pred(m)) return m;
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
    try { return await fn(client); }
    finally { try { client.close(); } catch {} if (child.exitCode === null) { child.kill(); await sleep(100); } }
}

test('plugin/script ERROR log reaches the WS log channel + recent_errors', { timeout: 180000 }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xinsp2-p13-'));
    const script = join(dir, 'logger.cpp');
    writeFileSync(script,
        '#include <xi/xi.hpp>\n' +
        '#include <xi/xi_use.hpp>\n' +
        'XI_SCRIPT_EXPORT\n' +
        'void xi_inspect_entry(int frame) {\n' +
        '    // No such instance -> host_api->log(ERROR) via warn_use_miss_ (the P1-3 path).\n' +
        '    // (v12: exchange() drives the miss; the Record process overload is gone.)\n' +
        '    xi::use("__p1_3_no_such_instance__").exchange("{\\"command\\":\\"ping\\"}");\n' +
        '}\n');

    await withBackend(async (c) => {
        await c.next(); // hello

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: script } });
        const comp = await c.rsp(1);
        assert.equal(comp.ok, true, 'compile ok (data: ' + JSON.stringify(comp.data) + ')');

        // Run once — the script's xi::use() miss logs an ERROR through host_api->log.
        c.send({ type: 'cmd', id: 2, name: 'run' });

        // The forwarded ERROR arrives as a live WS log message.
        const logMsg = await c.until(
            (m) => m.type === 'log' && m.level === 'error' &&
                   typeof m.msg === 'string' && m.msg.includes('no such instance'),
            30000);
        assert.ok(logMsg, 'a WS log{level:error} with the use-miss message arrived');

        // And it's retained in the recent-errors ring (re-pullable after the fact).
        c.send({ type: 'cmd', id: 3, name: 'recent_errors' });
        const re = await c.rsp(3);
        assert.equal(re.ok, true, 'recent_errors ok');
        const errs = re.data?.errors ?? re.data ?? [];
        const list = Array.isArray(errs) ? errs : (errs.errors ?? []);
        const hit = list.find((e) => typeof e.message === 'string' && e.message.includes('no such instance'));
        assert.ok(hit, 'recent_errors contains the forwarded plugin/script ERROR: ' + JSON.stringify(list).slice(0, 400));
    });
});
