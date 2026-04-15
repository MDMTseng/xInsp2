// M2 integration test — drive xinsp-backend.exe over real WebSocket.
//
// Spawns the service on a random port, sends ping / version / shutdown,
// verifies rsp shapes, asserts the process exits cleanly.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(
    __dirname,
    '../../backend/build/Release/xinsp-backend.exe'
);

function randomPort() {
    return 30000 + Math.floor(Math.random() * 20000);
}

// Wrapper that buffers incoming JSON messages so tests never miss the
// `hello` event between the ws 'open' and the test code attaching its
// first listener. Listener is registered at construction time, BEFORE
// the socket opens, so no race with server-initiated messages.
class BufferedClient {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.queue = [];
        this.waiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) return;
            let obj;
            try { obj = JSON.parse(data.toString()); }
            catch { return; }
            const w = this.waiters.shift();
            if (w) w.resolve(obj);
            else    this.queue.push(obj);
        });
    }
    opened() {
        return new Promise((resolve, reject) => {
            this.ws.once('open', resolve);
            this.ws.once('error', reject);
        });
    }
    send(obj) { this.ws.send(JSON.stringify(obj)); }
    next(timeoutMs = 5000) {
        if (this.queue.length) return Promise.resolve(this.queue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('nextJson timeout')), timeoutMs);
            this.waiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], {
        stdio: ['ignore', 'inherit', 'inherit'],
    });

    let client;
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        const c = new BufferedClient(`ws://127.0.0.1:${port}`);
        try {
            await c.opened();
            client = c;
            break;
        } catch {
            try { c.close(); } catch {}
        }
    }
    if (!client) {
        child.kill();
        throw new Error('failed to connect to backend');
    }

    try {
        return await fn(client, child, port);
    } finally {
        client.close();
        if (child.exitCode === null) {
            child.kill();
            await sleep(50);
        }
    }
}

test('hello event on connect', async () => {
    await withBackend(async (c) => {
        const hello = await c.next();
        assert.equal(hello.type, 'event');
        assert.equal(hello.name, 'hello');
        assert.equal(hello.data.abi, 1);
    });
});

test('cmd ping returns rsp with pong', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 1, name: 'ping' });
        const rsp = await c.next();
        assert.equal(rsp.type, 'rsp');
        assert.equal(rsp.id, 1);
        assert.equal(rsp.ok, true);
        assert.equal(rsp.data.pong, true);
        assert.ok(typeof rsp.data.ts === 'number');
    });
});

test('cmd version returns version string', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 2, name: 'version' });
        const rsp = await c.next();
        assert.equal(rsp.ok, true);
        assert.match(rsp.data.version, /^\d+\.\d+\.\d+/);
        assert.equal(rsp.data.abi, 1);
    });
});

test('unknown command returns ok:false', async () => {
    await withBackend(async (c) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 3, name: 'nope' });
        const rsp = await c.next();
        assert.equal(rsp.ok, false);
        assert.match(rsp.error, /unknown command/);
    });
});

test('shutdown makes backend exit cleanly', async () => {
    await withBackend(async (c, child) => {
        await c.next(); // hello
        c.send({ type: 'cmd', id: 9, name: 'shutdown' });
        const rsp = await c.next();
        assert.equal(rsp.ok, true);
        for (let i = 0; i < 20; ++i) {
            if (child.exitCode !== null) break;
            await sleep(100);
        }
        assert.equal(child.exitCode, 0);
    });
});
