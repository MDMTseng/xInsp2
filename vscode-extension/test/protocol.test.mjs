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

