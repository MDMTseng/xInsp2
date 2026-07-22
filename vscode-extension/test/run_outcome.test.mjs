// Run-outcome parsing/classification test — TypeScript side.
//
// Drives the pure runOutcome.ts model (no vscode dependency) against the same
// protocol/fixtures/*.json goldens the C++ and Python suites read, so the
// extension's verdict classification is proven against the real wire shapes.
// Requires Node's TS type-stripping (see the test:outcome script).
//
// Run: node --test test/run_outcome.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import {
    parseRunOutcome, parseRunFinished, outcomeClass, outcomeClassForCode,
    VerdictTally, RUN_RESULT_SCHEMA,
    XI_SYS_CRASHED, XI_SYS_NO_VERDICT, XI_SYS_DROPPED,
    CLASS_OK, CLASS_NG, CLASS_NA, CLASS_CRASHED, CLASS_NO_VERDICT, CLASS_DROPPED,
} from '../src/runOutcome.ts';

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = resolve(__dirname, '../../protocol/fixtures');
const load = (name) => JSON.parse(readFileSync(resolve(fixturesDir, name), 'utf8'));

test('run_result ok fixture: additive fields + class', () => {
    const o = parseRunOutcome(load('run_result.json'));
    assert.equal(o.code, 1);
    assert.equal(o.run_id, 17);
    assert.equal(o.schema, RUN_RESULT_SCHEMA);
    assert.equal(o.verdictClass, CLASS_OK);      // wire `class` → verdictClass
    assert.equal(outcomeClass(o), CLASS_OK);
    assert.equal(o.source, '@script');
    assert.equal(o.group, 'main');
    assert.equal(o.station_id, 'cell-A');
    assert.equal(o.script_generation, 4);
});

test('run_result crashed fixture classifies crashed', () => {
    const o = parseRunOutcome(load('run_result_crashed.json'));
    assert.equal(o.code, XI_SYS_CRASHED);
    assert.equal(outcomeClass(o), CLASS_CRASHED);
    assert.equal(o.reason_code, 'seh_access_violation');
});

test('run_result no_verdict fixture classifies no_verdict', () => {
    const o = parseRunOutcome(load('run_result_no_verdict.json'));
    assert.equal(o.code, XI_SYS_NO_VERDICT);
    assert.equal(outcomeClass(o), CLASS_NO_VERDICT);
});

test('outcomeClassForCode mirrors the backend bands', () => {
    assert.equal(outcomeClassForCode(5), CLASS_OK);
    assert.equal(outcomeClassForCode(0), CLASS_NA);
    assert.equal(outcomeClassForCode(-1), CLASS_NG);
    assert.equal(outcomeClassForCode(-42), CLASS_NG);
    assert.equal(outcomeClassForCode(XI_SYS_DROPPED), CLASS_DROPPED);
    assert.equal(outcomeClassForCode(XI_SYS_CRASHED), CLASS_CRASHED);
    assert.equal(outcomeClassForCode(XI_SYS_NO_VERDICT), CLASS_NO_VERDICT);
    // Below the reserved band but not a known code → clamp to NA, never NG.
    assert.equal(outcomeClassForCode(-999999), CLASS_NA);
});

test('older event missing `class` falls back to code derivation', () => {
    const o = parseRunOutcome({ name: 'run_result', data: { code: -3, msg: 'edge chip', run_id: 9 } });
    assert.equal(o.verdictClass, undefined);
    assert.equal(outcomeClass(o), CLASS_NG);
});

test('tolerant of a bare data dict and of missing/garbage fields', () => {
    const o = parseRunOutcome({ code: 2 });          // bare data, not wrapped
    assert.equal(o.code, 2);
    assert.equal(o.msg, '');
    assert.equal(o.run_id, undefined);
    const empty = parseRunOutcome(null);             // hostile input → defaults
    assert.equal(empty.code, 0);
    assert.equal(outcomeClass(empty), CLASS_NA);
});

test('run_finished fixture parses compute timing', () => {
    const f = parseRunFinished(load('run_finished.json'));
    assert.equal(f.run_id, 17);
    assert.equal(f.ms, 12);
    assert.equal(f.inspect_compute_us, 11840);
});

test('VerdictTally rolls counts and tracks the last verdict', () => {
    const t = new VerdictTally();
    assert.equal(t.record(parseRunOutcome(load('run_result.json'))), CLASS_OK);
    assert.equal(t.record(parseRunOutcome(load('run_result_crashed.json'))), CLASS_CRASHED);
    assert.equal(t.record(parseRunOutcome(load('run_result_no_verdict.json'))), CLASS_NO_VERDICT);
    t.record(parseRunOutcome({ data: { code: -1, msg: 'ng' } }));
    assert.equal(t.ok, 1);
    assert.equal(t.crashed, 1);
    assert.equal(t.no_verdict, 1);
    assert.equal(t.ng, 1);
    assert.equal(t.total, 4);
    assert.equal(t.lastClass, CLASS_NG);
    t.reset();
    assert.equal(t.total, 0);
    assert.equal(t.last, undefined);
});
