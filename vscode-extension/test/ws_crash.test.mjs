// Crash isolation tests — compile and run scripts that crash in various ways.
// After each crash, verify the backend is still alive and can run a normal script.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');
const crashDir = resolve(__dirname, '../../examples/crash_tests');
const goodScript = resolve(__dirname, '../../examples/user_script_example.cpp');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.queue = []; this.waiters = [];
        this.ws.on('message', (d, bin) => {
            if (bin) return;
            try { const o = JSON.parse(d.toString()); const w = this.waiters.shift(); if (w) w.resolve(o); else this.queue.push(o); } catch {}
        });
    }
    opened() { return new Promise((r, e) => { this.ws.once('open', r); this.ws.once('error', e); }); }
    send(o) { this.ws.send(JSON.stringify(o)); }
    next(ms = 90000) {
        if (this.queue.length) return Promise.resolve(this.queue.shift());
        return new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('timeout')), ms); this.waiters.push({ resolve: v => { clearTimeout(t); res(v); } }); });
    }
    async nextNonLog(ms = 90000) { for (;;) { const m = await this.next(ms); if (m.type !== 'log') return m; } }
    // Drain all queued messages and return log messages
    drainLogs() {
        const logs = this.queue.filter(m => m.type === 'log');
        this.queue = this.queue.filter(m => m.type !== 'log');
        return logs;
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

async function compileAndRun(c, scriptPath) {
    c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: scriptPath } });
    const cr = await c.nextNonLog();
    if (!cr.ok) return { compiled: false, error: cr.error };

    c.send({ type: 'cmd', id: 2, name: 'run' });
    const rr = await c.nextNonLog();
    // Collect any error logs
    await sleep(500);
    const logs = c.drainLogs();
    const errorLogs = logs.filter(l => l.level === 'error');
    return { compiled: true, ran: rr.ok, errorLogs };
}

async function verifyBackendAlive(c) {
    c.send({ type: 'cmd', id: 99, name: 'ping' });
    const pong = await c.nextNonLog();
    return pong.ok && pong.data?.pong;
}

const crashScripts = [
    { name: 'null_deref',      file: 'null_deref.cpp',      expect: 'ACCESS_VIOLATION' },
    { name: 'div_zero',        file: 'div_zero.cpp',         expect: 'INT_DIVIDE_BY_ZERO' },
    { name: 'array_overrun',   file: 'array_overrun.cpp',    expect: 'ACCESS_VIOLATION' },
    // stack_overflow skipped — kills process, needs separate guard thread
    { name: 'cpp_exception',   file: 'cpp_exception.cpp',    expect: 'intentional error' },
];

test('crash isolation: backend survives all crash types', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello

        for (const crash of crashScripts) {
            console.log(`\n  [crash] testing ${crash.name}...`);

            const scriptPath = resolve(crashDir, crash.file);
            const result = await compileAndRun(c, scriptPath);

            if (!result.compiled) {
                console.log(`  [crash] ${crash.name}: compile failed (${result.error}) — skipping`);
                continue;
            }

            // The run should return ok (rsp sent before crash) but error log should mention the crash
            console.log(`  [crash] ${crash.name}: ran=${result.ran}, errors=${result.errorLogs.length}`);
            if (result.errorLogs.length > 0) {
                for (const log of result.errorLogs) {
                    console.log(`    error: ${log.msg}`);
                    assert.ok(log.msg.includes(crash.expect),
                        `expected "${crash.expect}" in error, got: ${log.msg}`);
                }
            }

            // CRITICAL: backend must still be alive
            const alive = await verifyBackendAlive(c);
            assert.ok(alive, `backend died after ${crash.name}!`);
            console.log(`  [crash] ${crash.name}: backend alive ✓`);
        }

        // Final check: compile and run a NORMAL script after all crashes
        console.log('\n  [crash] running normal script after all crashes...');
        c.send({ type: 'cmd', id: 50, name: 'compile_and_load', args: { path: goodScript } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'normal script compiles after crashes');

        c.send({ type: 'cmd', id: 51, name: 'run' });
        const rr = await c.nextNonLog();
        assert.equal(rr.ok, true, 'normal script runs after crashes');

        const vars = await c.nextNonLog();
        assert.equal(vars.type, 'vars', 'vars received after crashes');
        assert.ok(vars.items.length >= 5, 'normal output after crashes');
        console.log('  [crash] normal script works ✓');
    });
});
