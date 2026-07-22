// Health/state-contract parsing test — TypeScript side.
//
// Drives the pure healthState.ts model (no vscode dependency) against the shared
// protocol/fixtures/*.json goldens (the canonical get_health / health_changed
// shapes from docs/new_gen/04-health-contract.md, byte-mirrored by the backend's
// service_health.cpp + xi_health.hpp) plus synthesized edge cases, so the
// extension's health surfacing is proven against the real wire shapes.
// Requires Node's TS type-stripping (see the test:health script).
//
// Run: node --experimental-strip-types --test test/health_state.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

import {
    parseHealth, mergeHealthEvent, isProblemState, enteredProblem,
    failingComponents, componentSummary, summarizeFailing,
    HEALTH_SCHEMA,
    STATE_BOOT, STATE_PROJECT_LOADED, STATE_RUNNING, STATE_DEGRADED, STATE_DRAINING, STATE_FAULT,
    HEALTH_OK, HEALTH_DEGRADED, HEALTH_FAILED,
    REASON_PLUGIN_FAULT, REASON_COMPILE_ERROR,
} from '../src/healthState.ts';

const __dirname = dirname(fileURLToPath(import.meta.url));
const fixturesDir = resolve(__dirname, '../../protocol/fixtures');
const load = (name) => JSON.parse(readFileSync(resolve(fixturesDir, name), 'utf8'));

test('get_health fixture: snapshot + typed components + derived extras', () => {
    const s = parseHealth(load('get_health.json'));
    assert.equal(s.schema, HEALTH_SCHEMA);
    assert.equal(s.state, STATE_DEGRADED);
    assert.equal(s.boot_id, '9f3c1a2b');
    assert.equal(s.station_id, 'line3-cell2');
    assert.equal(s.components.length, 5);

    const cam0 = s.components.find((c) => c.kind === 'instance' && c.name === 'cam0');
    assert.equal(cam0.health, HEALTH_DEGRADED);
    assert.equal(cam0.reason_code, REASON_PLUGIN_FAULT);
    assert.equal(cam0.crash_count, 3);

    const group = s.components.find((c) => c.kind === 'group');
    assert.equal(group.queue_now, 0);
    assert.equal(group.running, 1);
    assert.equal(group.dropped, 0);

    const src = s.components.find((c) => c.kind === 'source');
    assert.equal(src.last_emit_age_ms, 41);
    // A get_health snapshot lists all in `components` and carries no single `component`.
    assert.equal(s.component, undefined);
});

test('failingComponents / summaries pick only the non-ok set (failed first)', () => {
    const s = parseHealth(load('get_health.json'));
    const bad = failingComponents(s);
    assert.equal(bad.length, 1);
    assert.equal(bad[0].name, 'cam0');
    assert.equal(componentSummary(bad[0]), 'instance "cam0": degraded (plugin_fault)');
    assert.equal(summarizeFailing(s), 'instance "cam0": degraded (plugin_fault)');
});

test('failed components sort ahead of degraded in the failing set', () => {
    const s = parseHealth({
        state: STATE_DEGRADED,
        components: [
            { kind: 'instance', name: 'a', health: HEALTH_DEGRADED, reason_code: REASON_PLUGIN_FAULT },
            { kind: 'script',   name: 'inspect.cpp', health: HEALTH_FAILED, reason_code: REASON_COMPILE_ERROR },
            { kind: 'instance', name: 'b', health: HEALTH_OK, reason_code: '' },
        ],
    });
    const bad = failingComponents(s);
    assert.deepEqual(bad.map((c) => c.name), ['inspect.cpp', 'a']);
    assert.equal(summarizeFailing(s),
        'script "inspect.cpp": failed (compile_error), instance "a": degraded (plugin_fault)');
});

test('health_changed event with a component parses (unwraps .data)', () => {
    const s = parseHealth(load('health_changed.json'));
    assert.equal(s.state, STATE_DEGRADED);
    assert.equal(s.ts_ms, 1751430000123);
    assert.ok(s.component, 'carries the single changed component');
    assert.equal(s.component.kind, 'instance');
    assert.equal(s.component.name, 'cam0');
    assert.equal(s.component.health, HEALTH_DEGRADED);
    assert.equal(s.component.reason_code, REASON_PLUGIN_FAULT);
    // Event form carries no full component list.
    assert.equal(s.components.length, 0);
});

test('pure state-transition health_changed carries no component', () => {
    const s = parseHealth(load('health_changed_state.json'));
    assert.equal(s.state, STATE_RUNNING);
    assert.equal(s.component, undefined);
    assert.equal(s.components.length, 0);
});

test('mergeHealthEvent overlays a changed component onto the last full snapshot', () => {
    const snap = parseHealth(load('get_health.json'));   // cam0 already degraded
    // A recovery event for cam0: state back to running, component now ok.
    const recover = parseHealth({
        state: STATE_RUNNING, since_ms: 1751430005000,
        component: { kind: 'instance', name: 'cam0', health: HEALTH_OK, reason_code: '' },
        ts_ms: 1751430005000,
    });
    const merged = mergeHealthEvent(snap, recover);
    assert.equal(merged.state, STATE_RUNNING);
    // Full component set is preserved (5), cam0 upserted to ok → nothing failing.
    assert.equal(merged.components.length, 5);
    assert.equal(failingComponents(merged).length, 0);
    // Identity from the prior snapshot is carried through the event.
    assert.equal(merged.station_id, 'line3-cell2');

    // A fault event for a NEW component appends it (kept, not replaced).
    const newFault = parseHealth({
        state: STATE_DEGRADED,
        component: { kind: 'instance', name: 'sink0', health: HEALTH_FAILED, reason_code: 'prepare_failed' },
    });
    const merged2 = mergeHealthEvent(merged, newFault);
    assert.equal(merged2.components.length, 5);   // sink0 already present → upsert, not append
    const sink = merged2.components.find((c) => c.name === 'sink0');
    assert.equal(sink.health, HEALTH_FAILED);
    assert.equal(summarizeFailing(merged2), 'instance "sink0": failed (prepare_failed)');
});

test('mergeHealthEvent with no prior snapshot stands on the event alone', () => {
    const ev = parseHealth(load('health_changed.json'));
    const merged = mergeHealthEvent(undefined, ev);
    assert.equal(merged.state, STATE_DEGRADED);
    assert.equal(merged.components.length, 1);   // just the event's own component
    assert.equal(merged.components[0].name, 'cam0');
});

test('isProblemState flags only degraded + fault', () => {
    assert.equal(isProblemState(STATE_DEGRADED), true);
    assert.equal(isProblemState(STATE_FAULT), true);
    assert.equal(isProblemState(STATE_RUNNING), false);
    assert.equal(isProblemState(STATE_BOOT), false);
    assert.equal(isProblemState(STATE_PROJECT_LOADED), false);
    assert.equal(isProblemState(STATE_DRAINING), false);
    assert.equal(isProblemState(undefined), false);
    assert.equal(isProblemState('some_future_state'), false);
});

test('enteredProblem fires on transition INTO a problem, not on staying', () => {
    assert.equal(enteredProblem(STATE_RUNNING, STATE_DEGRADED), true);   // running → degraded
    assert.equal(enteredProblem(STATE_DEGRADED, STATE_FAULT), true);     // degraded → fault (escalation)
    assert.equal(enteredProblem(STATE_DEGRADED, STATE_DEGRADED), false); // component-only change
    assert.equal(enteredProblem(STATE_DEGRADED, STATE_RUNNING), false);  // recovered
    assert.equal(enteredProblem(undefined, STATE_FAULT), true);          // first pull already faulted
    assert.equal(enteredProblem(undefined, STATE_RUNNING), false);
});

test('unknown state/health strings pass through as literals (forward-tolerant)', () => {
    const s = parseHealth({ state: 'quiescing', components: [
        { kind: 'instance', name: 'x', health: 'quarantined', reason_code: 'novel_reason' },
    ] });
    assert.equal(s.state, 'quiescing');
    assert.equal(s.components[0].health, 'quarantined');
    // A non-ok literal still counts as failing, so a newer health value isn't hidden.
    assert.equal(failingComponents(s).length, 1);
});

test('tolerant of a bare data dict and of missing/garbage input', () => {
    const bare = parseHealth({ state: STATE_RUNNING });   // no components key
    assert.equal(bare.state, STATE_RUNNING);
    assert.deepEqual(bare.components, []);

    const empty = parseHealth(null);                      // hostile input → safe defaults
    assert.equal(empty.state, STATE_BOOT);
    assert.deepEqual(empty.components, []);
    assert.equal(summarizeFailing(empty), '');

    const junkComps = parseHealth({ state: STATE_RUNNING, components: [null, 7, 'x', {}] });
    // null/number/string entries are dropped; the {} parses to an ok-defaulted component.
    assert.equal(junkComps.components.length, 1);
    assert.equal(junkComps.components[0].health, HEALTH_OK);
});
