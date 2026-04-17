// Shared test client — one place, all tests import from here.

import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const backendExe = resolve(__dirname, '../../../backend/build/Release/xinsp-backend.exe');
export const examplesDir = resolve(__dirname, '../../../examples');
export const scriptPath = (name) => resolve(examplesDir, name);

export function randomPort() {
    return 30000 + Math.floor(Math.random() * 20000);
}

export class Client {
    constructor(url) {
        this.ws = new WebSocket(url);
        this.textQueue = [];
        this.binaryQueue = [];
        this.textWaiters = [];
        this.binaryWaiters = [];
        this.ws.on('message', (data, isBinary) => {
            if (isBinary) {
                const w = this.binaryWaiters.shift();
                if (w) w.resolve(data);
                else this.binaryQueue.push(data);
            } else {
                try {
                    const obj = JSON.parse(data.toString());
                    const w = this.textWaiters.shift();
                    if (w) w.resolve(obj);
                    else this.textQueue.push(obj);
                } catch {}
            }
        });
    }

    opened() {
        return new Promise((res, rej) => {
            this.ws.once('open', res);
            this.ws.once('error', rej);
        });
    }

    send(obj) { this.ws.send(JSON.stringify(obj)); }

    nextText(ms = 90000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('text timeout')), ms);
            this.textWaiters.push({ resolve: v => { clearTimeout(t); resolve(v); } });
        });
    }

    async nextNonLog(ms = 90000) {
        for (;;) {
            const m = await this.nextText(ms);
            if (m.type !== 'log' && m.type !== 'event') return m;
        }
    }

    nextBinary(ms = 5000) {
        if (this.binaryQueue.length) return Promise.resolve(this.binaryQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('binary timeout')), ms);
            this.binaryWaiters.push({ resolve: v => { clearTimeout(t); resolve(v); } });
        });
    }

    // Drain all queued text messages
    drainText() {
        const all = [...this.textQueue];
        this.textQueue = [];
        return all;
    }

    drainLogs() {
        const logs = this.textQueue.filter(m => m.type === 'log');
        this.textQueue = this.textQueue.filter(m => m.type !== 'log');
        return logs;
    }

    // Wait until a condition is met (poll-based, avoids sleep+assert flakiness)
    async waitFor(conditionFn, { timeoutMs = 10000, pollMs = 100 } = {}) {
        const t0 = Date.now();
        while (Date.now() - t0 < timeoutMs) {
            if (conditionFn()) return true;
            await sleep(pollMs);
        }
        return false;
    }

    close() { try { this.ws.close(); } catch {} }
}

// Spawn backend, connect, run fn, clean up. Handles all lifecycle.
export async function withBackend(fn, { port } = {}) {
    port = port || randomPort();
    const child = spawn(backendExe, [`--port=${port}`], {
        stdio: ['ignore', 'inherit', 'inherit'],
    });
    let client;
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        client = new Client(`ws://127.0.0.1:${port}`);
        try { await client.opened(); break; }
        catch { try { client.close(); } catch {} client = null; }
    }
    if (!client) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(client, child, port); }
    finally {
        client.close();
        if (child.exitCode === null) { child.kill(); await sleep(100); }
    }
}

// Helper: compile a script and return the rsp (skipping logs + vars + events)
export async function compileScript(c, path) {
    c.send({ type: 'cmd', id: 100, name: 'compile_and_load', args: { path } });
    for (;;) {
        const m = await c.nextText();
        if (m.type === 'rsp') return m;
        // skip logs, vars, events, instances — all can arrive during compile
    }
}

// Helper: run inspection and return { rsp, vars }
export async function runInspection(c) {
    c.send({ type: 'cmd', id: 101, name: 'run' });
    const rsp = await c.nextNonLog();
    if (!rsp.ok) return { rsp, vars: null };
    const vars = await c.nextNonLog();
    return { rsp, vars };
}
