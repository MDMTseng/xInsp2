// Version-skew classification test — TypeScript side.
//
// Drives the pure versionCompat.ts model (no vscode dependency) against the
// shared protocol/fixtures/hello.json golden plus synthesized skew variants, so
// the extension's compatibility verdict is proven against the real `hello` wire
// shape. Requires Node's TS type-stripping (see the test:compat script).
//
// Run: node --test test/version_compat.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import {
    classifyHello, parseSemVer,
    EXPECTED_BACKEND_MAJOR, EXPECTED_BACKEND_MINOR, EXPECTED_WS_ABI,
} from '../src/versionCompat.ts';

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = resolve(__dirname, '../../protocol/fixtures');
const load = (name) => JSON.parse(readFileSync(resolve(fixturesDir, name), 'utf8'));

const EXT = '0.2.0';
// A backend version that sits exactly on the known-compatible row.
const OK = `${EXPECTED_BACKEND_MAJOR}.${EXPECTED_BACKEND_MINOR}.0`;
const hello = (data) => ({ type: 'event', name: 'hello', data });

test('sanity: the module expectations match README known-compatible row (0.2.x @ abi 1)', () => {
    assert.equal(EXPECTED_BACKEND_MAJOR, 0);
    assert.equal(EXPECTED_BACKEND_MINOR, 2);
    assert.equal(EXPECTED_WS_ABI, 1);
});

test('hello.json fixture classifies compatible and stays silent', () => {
    const r = classifyHello(load('hello.json'), EXT);
    assert.equal(r.kind, 'compatible');
    assert.equal(r.level, 'ok');
    assert.equal(r.backendVersion, '0.2.0');
    assert.equal(r.backendAbi, 1);
    assert.equal(r.advice, undefined);   // silence is the feature
    assert.match(r.summary, /compatible/i);
});

test('patch difference inside the expected minor is still compatible', () => {
    const r = classifyHello(hello({ version: '0.2.9', abi: 1 }), EXT);
    assert.equal(r.kind, 'compatible');
    assert.equal(r.level, 'ok');
});

test('accepts a bare data dict (not wrapped in an event envelope)', () => {
    const r = classifyHello({ version: OK, abi: 1 }, EXT);
    assert.equal(r.kind, 'compatible');
});

test('newer backend minor -> notice, proceed (names both versions)', () => {
    const r = classifyHello(hello({ version: '0.3.0', abi: 1 }), EXT);
    assert.equal(r.kind, 'newer');
    assert.equal(r.level, 'notice');
    assert.match(r.summary, /0\.3\.0/);
    assert.match(r.summary, new RegExp(`v${EXT.replace(/\./g, '\\.')}`));
    assert.ok(r.advice);
});

test('newer backend major -> notice', () => {
    const r = classifyHello(hello({ version: '1.0.0', abi: 1 }), EXT);
    assert.equal(r.kind, 'newer');
    assert.equal(r.level, 'notice');
});

test('older backend minor -> warn (asymmetric bad case)', () => {
    const r = classifyHello(hello({ version: '0.1.0', abi: 1 }), EXT);
    assert.equal(r.kind, 'older');
    assert.equal(r.level, 'warn');
    assert.match(r.summary, /0\.1\.0/);
    assert.match(r.advice ?? '', /Update the backend/i);
});

test('abi stamp mismatch -> warn regardless of a compatible version string', () => {
    const r = classifyHello(hello({ version: OK, abi: 2 }), EXT);
    assert.equal(r.kind, 'abi');
    assert.equal(r.level, 'warn');
    assert.equal(r.backendAbi, 2);
    assert.match(r.summary, /abi 2/);
    assert.match(r.summary, /abi 1/);
});

test('missing version -> unknown, notice, never blocks', () => {
    const r = classifyHello(hello({ commit: 'deadbee' }), EXT);
    assert.equal(r.kind, 'unknown');
    assert.equal(r.level, 'notice');
    assert.equal(r.backendVersion, undefined);
});

test('garbage version string -> unknown, notice', () => {
    const r = classifyHello(hello({ version: 'not-a-version', abi: 1 }), EXT);
    assert.equal(r.kind, 'unknown');
    assert.equal(r.level, 'notice');
});

test('missing abi alone (old backend, good version) is not a mismatch', () => {
    const r = classifyHello(hello({ version: OK }), EXT);
    assert.equal(r.kind, 'compatible');
    assert.equal(r.backendAbi, undefined);
});

test('hostile input (null / empty) -> unknown, never throws', () => {
    assert.equal(classifyHello(null, EXT).kind, 'unknown');
    assert.equal(classifyHello({}, EXT).kind, 'unknown');
    assert.equal(classifyHello(hello({}), EXT).kind, 'unknown');
});

test('parseSemVer tolerates pre-release / build suffixes and rejects junk', () => {
    assert.deepEqual(parseSemVer('0.2.0'), { major: 0, minor: 2, patch: 0 });
    assert.deepEqual(parseSemVer('0.2.0-rc1'), { major: 0, minor: 2, patch: 0 });
    assert.deepEqual(parseSemVer('1.4.7+abc'), { major: 1, minor: 4, patch: 7 });
    assert.deepEqual(parseSemVer('0.2'), { major: 0, minor: 2, patch: 0 });
    assert.equal(parseSemVer('v0.2.0'), undefined);   // leading v not stripped
    assert.equal(parseSemVer(''), undefined);
    assert.equal(parseSemVer(undefined), undefined);
    assert.equal(parseSemVer(42), undefined);
});
