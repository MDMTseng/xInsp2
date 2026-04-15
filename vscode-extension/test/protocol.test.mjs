// Protocol round-trip test — TypeScript side.
// Reads the same fixtures the C++ test_protocol.cpp reads and asserts the
// decoded shapes match. Uses plain `node --test` so no extra deps.
//
// Run: node --test vscode-extension/test/protocol.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = resolve(__dirname, '../../protocol/fixtures');

test('cmd_run fixture parses as a Cmd', () => {
    const text = readFileSync(resolve(fixturesDir, 'cmd_run.json'), 'utf8');
    const msg = JSON.parse(text);
    assert.equal(msg.type, 'cmd');
    assert.equal(msg.id, 1);
    assert.equal(msg.name, 'run');
    assert.equal(msg.args.frame_path, 'C:/images/sample.bmp');
});

test('vars_mixed fixture parses with expected kinds', () => {
    const text = readFileSync(resolve(fixturesDir, 'vars_mixed.json'), 'utf8');
    const msg = JSON.parse(text);
    assert.equal(msg.type, 'vars');
    assert.equal(msg.run_id, 17);
    assert.equal(msg.items.length, 4);
    assert.equal(msg.items[0].kind, 'image');
    assert.equal(msg.items[0].gid, 100);
    assert.equal(msg.items[1].kind, 'number');
    assert.equal(msg.items[1].value, 42);
    assert.equal(msg.items[2].kind, 'string');
    assert.equal(msg.items[2].value, 'ok');
    assert.equal(msg.items[3].kind, 'boolean');
    assert.equal(msg.items[3].value, true);
});

test('preview header big-endian round-trip', async () => {
    // Tiny inline impl mirroring protocol.ts so this file doesn't need a
    // TS build step. Keep in sync with vscode-extension/src/protocol.ts.
    const PREVIEW_HEADER_SIZE = 20;
    const encode = (h) => {
        const buf = new Uint8Array(PREVIEW_HEADER_SIZE);
        const dv = new DataView(buf.buffer);
        dv.setUint32(0,  h.gid,      false);
        dv.setUint32(4,  h.codec,    false);
        dv.setUint32(8,  h.width,    false);
        dv.setUint32(12, h.height,   false);
        dv.setUint32(16, h.channels, false);
        return buf;
    };
    const decode = (buf) => {
        const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
        return {
            gid:      dv.getUint32(0,  false),
            codec:    dv.getUint32(4,  false),
            width:    dv.getUint32(8,  false),
            height:   dv.getUint32(12, false),
            channels: dv.getUint32(16, false),
        };
    };

    const h = { gid: 100, codec: 0, width: 4000, height: 5000, channels: 3 };
    const buf = encode(h);
    assert.equal(buf.byteLength, PREVIEW_HEADER_SIZE);
    // First byte of gid=100 is 0, last byte is 100 (big-endian).
    assert.equal(buf[0], 0);
    assert.equal(buf[3], 100);
    const back = decode(buf);
    assert.deepEqual(back, h);
});
