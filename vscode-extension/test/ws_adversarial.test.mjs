// ws_adversarial.test.mjs — Adversarial / edge-case tests.
//
// Covers: malformed JSON, missing fields, huge payload, rapid-fire pings,
// path injection, double start, double stop, empty compile path.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import WebSocket from 'ws';
import {
    withBackend, compileScript, scriptPath, randomPort, Client
} from './helpers/client.mjs';

// ---------------------------------------------------------------
// 1. Malformed JSON — backend stays alive
// ---------------------------------------------------------------
test('malformed JSON does not crash backend', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Send raw garbage (not valid JSON)
        c.ws.send('this is not json {{{');
        await sleep(500);

        // Backend should still respond to ping
        c.send({ type: 'cmd', id: 1, name: 'ping' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'backend alive after malformed JSON');
        assert.equal(rsp.data.pong, true);
    });
});

// ---------------------------------------------------------------
// 2. Missing cmd fields — error, not crash
// ---------------------------------------------------------------
test('missing cmd fields returns error without crash', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Send cmd with no id or name
        c.ws.send(JSON.stringify({ type: 'cmd' }));
        await sleep(500);

        // Backend should still be alive
        c.send({ type: 'cmd', id: 1, name: 'ping' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'backend alive after missing fields');
    });
});

// ---------------------------------------------------------------
// 3. Huge payload — rejected or handled, no OOM
// ---------------------------------------------------------------
test('huge payload (1MB) does not crash backend', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Build a 1MB JSON string
        const bigString = 'A'.repeat(1024 * 1024);
        const payload = JSON.stringify({
            type: 'cmd', id: 999, name: 'ping', args: { data: bigString }
        });

        c.ws.send(payload);
        await sleep(1000);

        // Backend should still respond
        c.send({ type: 'cmd', id: 1, name: 'ping' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'backend alive after huge payload');
    });
});

// ---------------------------------------------------------------
// 4. Rapid-fire 100 pings — all get responses
// ---------------------------------------------------------------
test('rapid-fire 100 pings all receive responses', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        const COUNT = 100;

        // Fire all pings as fast as possible
        for (let i = 0; i < COUNT; ++i) {
            c.send({ type: 'cmd', id: 1000 + i, name: 'ping' });
        }

        // Collect all responses
        const responses = [];
        for (let i = 0; i < COUNT; ++i) {
            const rsp = await c.nextNonLog(10000);
            responses.push(rsp);
        }

        assert.equal(responses.length, COUNT,
            `should receive ${COUNT} responses, got ${responses.length}`);

        const allOk = responses.every(r => r.ok === true);
        assert.ok(allOk, 'all responses should be ok:true');

        // Verify all IDs were answered
        const ids = new Set(responses.map(r => r.id));
        for (let i = 0; i < COUNT; ++i) {
            assert.ok(ids.has(1000 + i), `response for id ${1000 + i} missing`);
        }
    });
});

// ---------------------------------------------------------------
// 5. Path injection in compile_and_load — rejected
// ---------------------------------------------------------------
test('path injection with & in compile path is rejected', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Path with command injection attempt
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load',
                 args: { path: 'C:/foo & del /q C:/*.* & echo.cpp' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, false, 'path injection should be rejected');
        assert.ok(rsp.error, 'should have error message');

        // Backend still alive
        c.send({ type: 'cmd', id: 2, name: 'ping' });
        const pr = await c.nextNonLog();
        assert.equal(pr.ok, true, 'backend alive after path injection attempt');
    });
});

// ---------------------------------------------------------------
// 6. Double start — second returns already:true or handles gracefully
// ---------------------------------------------------------------
test('double start is handled gracefully', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr.ok, true);

        // First start
        c.send({ type: 'cmd', id: 1, name: 'start', args: { fps: 5 } });
        const r1 = await c.nextNonLog();
        assert.equal(r1.ok, true, 'first start ok');

        await sleep(300);
        c.drainText();

        // Second start — should not crash
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 5 } });
        const r2 = await c.nextNonLog();
        // May return ok:true (restart) or ok:true with already:true
        // Either is acceptable as long as no crash
        assert.equal(r2.type, 'rsp', 'should get a response');

        await sleep(300);
        c.drainText();

        // Stop and verify backend alive
        c.send({ type: 'cmd', id: 3, name: 'stop' });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true, 'stop ok after double start');

        c.send({ type: 'cmd', id: 4, name: 'ping' });
        const pr = await c.nextNonLog();
        assert.equal(pr.ok, true, 'backend alive after double start');
    });
});

// ---------------------------------------------------------------
// 7. Double stop — no crash
// ---------------------------------------------------------------
test('double stop without start does not crash', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Stop without ever starting
        c.send({ type: 'cmd', id: 1, name: 'stop' });
        const r1 = await c.nextNonLog();
        assert.equal(r1.ok, true, 'first stop ok (no-op)');

        // Stop again
        c.send({ type: 'cmd', id: 2, name: 'stop' });
        const r2 = await c.nextNonLog();
        assert.equal(r2.ok, true, 'second stop ok (no-op)');

        // Backend still alive
        c.send({ type: 'cmd', id: 3, name: 'ping' });
        const pr = await c.nextNonLog();
        assert.equal(pr.ok, true, 'backend alive after double stop');
    });
});

// ---------------------------------------------------------------
// 8. compile_and_load with empty path — error, not crash
// ---------------------------------------------------------------
test('compile_and_load with empty path returns error', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'compile_and_load',
                 args: { path: '' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, false, 'empty path should fail');
        assert.ok(rsp.error, 'should have error message');

        // Backend still alive
        c.send({ type: 'cmd', id: 2, name: 'ping' });
        const pr = await c.nextNonLog();
        assert.equal(pr.ok, true, 'backend alive after empty path compile');
    });
});
