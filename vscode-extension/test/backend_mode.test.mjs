// backend_mode.test.mjs — unit for the FE/BE ownership decision.
//
// The extension must NOT spawn/respawn a backend the xinsp-fe supervisor owns
// (attach mode), and must own it itself in managed mode. This pins the pure
// resolveBackendMode() decision (the one piece of that logic testable without
// the VS Code extension host).
//
// Run: node --test vscode-extension/test/backend_mode.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolveBackendMode } from '../src/backendMode.mjs';

test('explicit managed always manages (ignores the port probe)', () => {
    assert.equal(resolveBackendMode('managed', false), 'managed');
    assert.equal(resolveBackendMode('managed', true), 'managed');  // even if a BE is up
});

test('explicit attach always attaches (ignores the port probe)', () => {
    assert.equal(resolveBackendMode('attach', false), 'attach');
    assert.equal(resolveBackendMode('attach', true), 'attach');
});

test('auto attaches iff a backend is already listening', () => {
    assert.equal(resolveBackendMode('auto', true), 'attach');
    assert.equal(resolveBackendMode('auto', false), 'managed');
});

test('unknown / empty mode behaves like the default (auto)', () => {
    assert.equal(resolveBackendMode('', true), 'attach');
    assert.equal(resolveBackendMode(undefined, false), 'managed');
    assert.equal(resolveBackendMode('bogus', true), 'attach');
});
