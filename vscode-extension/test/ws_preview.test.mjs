// M4 integration test — run demo inspection and verify the JPEG preview
// binary frame arrives with the expected header + a valid JPEG payload.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import WebSocket from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, '../../backend/build/Release/xinsp-backend.exe');

function randomPort() { return 30000 + Math.floor(Math.random() * 20000); }

class BufferedClient {
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
                else   this.binaryQueue.push(data);
            } else {
                try {
                    const obj = JSON.parse(data.toString());
                    const w = this.textWaiters.shift();
                    if (w) w.resolve(obj);
                    else   this.textQueue.push(obj);
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
    nextText(timeoutMs = 5000) {
        if (this.textQueue.length) return Promise.resolve(this.textQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('nextText timeout')), timeoutMs);
            this.textWaiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    nextBinary(timeoutMs = 5000) {
        if (this.binaryQueue.length) return Promise.resolve(this.binaryQueue.shift());
        return new Promise((resolve, reject) => {
            const t = setTimeout(() => reject(new Error('nextBinary timeout')), timeoutMs);
            this.binaryWaiters.push({ resolve: (v) => { clearTimeout(t); resolve(v); } });
        });
    }
    close() { try { this.ws.close(); } catch {} }
}

async function withBackend(fn) {
    const port = randomPort();
    const child = spawn(backendExe, [`--port=${port}`], { stdio: ['ignore', 'inherit', 'inherit'] });
    let client;
    for (let i = 0; i < 30; ++i) {
        await sleep(100);
        const c = new BufferedClient(`ws://127.0.0.1:${port}`);
        try { await c.opened(); client = c; break; } catch { try { c.close(); } catch {} }
    }
    if (!client) { child.kill(); throw new Error('connect failed'); }
    try { return await fn(client, child); }
    finally {
        client.close();
        if (child.exitCode === null) { child.kill(); await sleep(50); }
    }
}

function decodePreviewHeader(buf) {
    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
    return {
        gid:      dv.getUint32(0,  false),
        codec:    dv.getUint32(4,  false),
        width:    dv.getUint32(8,  false),
        height:   dv.getUint32(12, false),
        channels: dv.getUint32(16, false),
    };
}

test('run emits JPEG preview frame for an image variable', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello
        c.send({ type: 'cmd', id: 1, name: 'run' });

        const rsp = await c.nextText();
        assert.equal(rsp.ok, true);

        const vars = await c.nextText();
        assert.equal(vars.type, 'vars');
        const img = vars.items.find(i => i.name === 'frame_img');
        assert.ok(img, 'frame_img should be present');
        assert.equal(img.kind, 'image');
        assert.equal(typeof img.gid, 'number');

        const frame = await c.nextBinary();
        assert.ok(frame.length > 20, 'preview frame too short');

        const hdr = decodePreviewHeader(frame);
        assert.equal(hdr.gid, img.gid, 'gid mismatch');
        assert.equal(hdr.codec, 0, 'codec should be JPEG=0');
        assert.equal(hdr.width, 64);
        assert.equal(hdr.height, 48);
        assert.equal(hdr.channels, 3);

        // Validate JPEG magic bytes: FF D8 FF ... FF D9
        const jpg = frame.subarray(20);
        assert.equal(jpg[0], 0xFF);
        assert.equal(jpg[1], 0xD8);
        assert.equal(jpg[2], 0xFF);
        assert.equal(jpg[jpg.length - 2], 0xFF);
        assert.equal(jpg[jpg.length - 1], 0xD9);
    });
});
