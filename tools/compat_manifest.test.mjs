// compat_manifest.test.mjs — self-check for the in-artifact compat manifest.
//
// Validates that tools/compat_manifest.mjs produces a COMPLETE, coherent
// manifest WITHOUT cutting a full release: every field resolves (no nulls),
// versions parse as semver, the ABI numbers match xi_abi.h read independently,
// and the tools/compat-matrix.json authority still matches the real per-package
// version sources. Wired into ctest as `compat_manifest` so a version-constant
// move (backend, ABI, extension, python) that forgets to update the matrix or
// breaks a parser fails LOUDLY here instead of silently stamping a null-field
// manifest into a shipped zip.
//
// Run:  node --test tools/compat_manifest.test.mjs
//
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import {
    ROOT, buildManifest, parseAbi, parseBackendVersion, parseWsAbi,
    readPackageVersion, parsePyprojectVersion, readMatrix, WS_TABLE_MIN,
} from './compat_manifest.mjs';

// A strict semver-ish check (major.minor.patch, optional pre-release/build).
const SEMVER = /^\d+\.\d+\.\d+(?:[-+].*)?$/;

// Walk the manifest and collect the dotted path of every null/undefined leaf.
function nullPaths(obj, prefix = '') {
    const bad = [];
    for (const [k, v] of Object.entries(obj)) {
        const path = prefix ? `${prefix}.${k}` : k;
        if (v === null || v === undefined) bad.push(path);
        else if (typeof v === 'object' && !Array.isArray(v)) bad.push(...nullPaths(v, path));
        else if (Array.isArray(v)) {
            v.forEach((el, i) => {
                if (el === null || el === undefined) bad.push(`${path}[${i}]`);
                else if (typeof el === 'object') bad.push(...nullPaths(el, `${path}[${i}]`));
            });
        }
    }
    return bad;
}

test('every manifest field resolves (no nulls)', () => {
    // Pin the timestamp so the test is about resolution, not the clock, and pass
    // an explicit sha so a non-git sandbox does not spuriously null built_from.
    const m = buildManifest(ROOT, { timestamp: '2026-01-01T00:00:00.000Z', gitSha: 'test-sha' });
    const bad = nullPaths(m);
    assert.deepEqual(bad, [], `null/undefined manifest field(s): ${bad.join(', ')}`);
});

test('built_from resolves to a real git sha in this checkout', () => {
    const m = buildManifest(ROOT, { timestamp: '2026-01-01T00:00:00.000Z' });
    assert.match(m.built_from, /^[0-9a-f]{7,40}$/, 'built_from is not a git sha');
});

test('all versions parse as semver', () => {
    const m = buildManifest(ROOT, { gitSha: 'x' });
    for (const [label, v] of [
        ['artifact.version', m.artifact.version],
        ['backend_version', m.backend_version],
        ['extension', m.known_compatible.extension.version],
        ['ui_components', m.known_compatible.ui_components],
        ['python_client', m.known_compatible.python_client.version],
    ]) {
        assert.match(v, SEMVER, `${label} is not semver: ${v}`);
    }
});

test('plugin ABI numbers match xi_abi.h read independently', () => {
    const m = buildManifest(ROOT, { gitSha: 'x' });
    const abi = parseAbi(ROOT);
    assert.equal(m.plugin_abi.version, abi.version);
    assert.equal(m.plugin_abi.min_compat, abi.min_compat);
    assert.ok(Number.isInteger(m.plugin_abi.version) && m.plugin_abi.version > 0);
});

test('ws_protocol.abi matches service_main.cpp, command_count is plausible', () => {
    const m = buildManifest(ROOT, { gitSha: 'x' });
    assert.equal(m.ws_protocol.abi, parseWsAbi(ROOT));
    assert.ok(Number.isInteger(m.ws_protocol.command_count));
    assert.ok(m.ws_protocol.command_count >= WS_TABLE_MIN,
        `command_count ${m.ws_protocol.command_count} below floor ${WS_TABLE_MIN}`);
    assert.ok(m.ws_protocol.schemas.length > 0, 'no contract schemas found');
    for (const s of m.ws_protocol.schemas) {
        assert.ok(s.$id, `schema ${s.file} has no $id (unreadable or malformed)`);
    }
});

// The matrix is the SINGLE authority for the tested-together set. If any pinned
// value drifts from its real source file, the matrix is stale — fail so the next
// version bump cannot ship a manifest that disagrees with the binaries.
test('compat-matrix.json pins match the real per-package version sources', () => {
    const kc = readMatrix(ROOT).known_compatible;
    assert.equal(kc.backend, parseBackendVersion(ROOT), 'matrix backend != CMakeLists');
    assert.equal(kc.extension, readPackageVersion(ROOT, 'vscode-extension/package.json'),
        'matrix extension != vscode-extension/package.json');
    assert.equal(kc.ui_components, readPackageVersion(ROOT, 'ui-components/package.json'),
        'matrix ui_components != ui-components/package.json');
    assert.equal(kc.python_client, parsePyprojectVersion(ROOT),
        'matrix python_client != pyproject.toml');
    assert.equal(kc.plugin_abi, parseAbi(ROOT).version, 'matrix plugin_abi != xi_abi.h');
    assert.equal(kc.ws_protocol_abi, parseWsAbi(ROOT), 'matrix ws_protocol_abi != service_main.cpp');
});

// README's "Known-compatible set" table is prose, so drift is a WARNING, not a
// failure (regenerating the table from the matrix would be overreach for this
// change) — but we surface it so a stale row is visible rather than silent.
test('README known-compatible row vs matrix (advisory drift warning)', () => {
    const kc = readMatrix(ROOT).known_compatible;
    const readme = readFileSync(join(ROOT, 'README.md'), 'utf8');
    // Line-based (CRLF-tolerant): find the header row, skip the separator, read
    // the single data row of the "Known-compatible set" table.
    const lines = readme.split(/\r?\n/);
    const hdr = lines.findIndex((l) =>
        /\|\s*Backend\s*\|/.test(l) && /Extension/.test(l) && /ABI/.test(l));
    const dataLine = hdr !== -1 ? lines[hdr + 2] : undefined;
    if (!dataLine || !dataLine.includes('|')) {
        console.warn('[compat-manifest] NOTE: could not locate README known-compatible row to check.');
        return;
    }
    const cells = dataLine.replace(/^\||\|$/g, '').split('|').map((c) => c.trim());
    const [rBackend, rExt, rUi, rPy, rAbi] = cells;
    const expect = [
        ['backend', rBackend, kc.backend],
        ['extension', rExt, kc.extension],
        ['ui-components', rUi, kc.ui_components],
        ['python client', rPy, kc.python_client],
        ['ABI', rAbi.replace(/^v/i, ''), String(kc.plugin_abi)],
    ];
    const drift = expect.filter(([, got, want]) => got !== want);
    if (drift.length) {
        for (const [label, got, want] of drift) {
            console.warn(`[compat-manifest] README DRIFT: ${label} row says "${got}", matrix says "${want}".`);
        }
    }
    // Advisory only — never fails the gate.
    assert.ok(true);
});
