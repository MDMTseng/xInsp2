// ws_state.test.mjs — Tests for xi::state() persistence across runs and recompiles.
//
// Uses examples/use_demo.cpp which increments state["run_count"] each run
// and exposes it as VAR(run_count, ...).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import {
    withBackend, compileScript, runInspection, scriptPath
} from './helpers/client.mjs';

const USE_DEMO = scriptPath('use_demo.cpp');

// ---------------------------------------------------------------
// 1. state persists across runs — run_count increments each time
// ---------------------------------------------------------------
test('state persists across runs: run_count increments', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        const cr = await compileScript(c, USE_DEMO);
        assert.equal(cr.ok, true, 'compile ok');

        // Run 3 times and verify run_count increments
        for (let expected = 1; expected <= 3; ++expected) {
            const { rsp, vars } = await runInspection(c);
            assert.equal(rsp.ok, true, `run ${expected} ok`);
            assert.equal(vars.type, 'vars');

            const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));
            assert.ok(byName.run_count, `run_count var exists on run ${expected}`);
            assert.equal(byName.run_count.value, expected,
                `run_count should be ${expected}, got ${byName.run_count.value}`);
        }
    });
});

// ---------------------------------------------------------------
// 2. state survives hot-reload (recompile same script)
// ---------------------------------------------------------------
test('state survives hot-reload: recompile does not reset run_count', { timeout: 90000 },async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Compile and run once → run_count = 1
        const cr1 = await compileScript(c, USE_DEMO);
        assert.equal(cr1.ok, true);

        const r1 = await runInspection(c);
        assert.equal(r1.rsp.ok, true);
        const byName1 = Object.fromEntries(r1.vars.items.map(v => [v.name, v]));
        assert.equal(byName1.run_count.value, 1);

        // Recompile the same script (hot-reload)
        const cr2 = await compileScript(c, USE_DEMO);
        assert.equal(cr2.ok, true, 'recompile ok');

        // Run again → run_count should be 2, NOT reset to 1
        const r2 = await runInspection(c);
        assert.equal(r2.rsp.ok, true);
        const byName2 = Object.fromEntries(r2.vars.items.map(v => [v.name, v]));
        assert.equal(byName2.run_count.value, 2,
            'run_count should be 2 after recompile, state must survive hot-reload');
    });
});

// ---------------------------------------------------------------
// 3. state survives param change
// ---------------------------------------------------------------
test('state survives param change: set_param does not clear state', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        const cr = await compileScript(c, USE_DEMO);
        assert.equal(cr.ok, true);

        // Run once → run_count = 1
        const r1 = await runInspection(c);
        assert.equal(r1.rsp.ok, true);
        const byName1 = Object.fromEntries(r1.vars.items.map(v => [v.name, v]));
        assert.equal(byName1.run_count.value, 1);

        // Change a param
        c.send({ type: 'cmd', id: 10, name: 'set_param',
                 args: { name: 'threshold', value: 200 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true, 'set_param ok');

        // Run again → run_count should be 2 (state not cleared by param change)
        const r2 = await runInspection(c);
        assert.equal(r2.rsp.ok, true);
        const byName2 = Object.fromEntries(r2.vars.items.map(v => [v.name, v]));
        assert.equal(byName2.run_count.value, 2,
            'run_count should be 2 after param change, state must survive');
    });
});

// ---------------------------------------------------------------
// 4. state preserves nested Record structure
// ---------------------------------------------------------------
test('state preserves data across multiple runs with varying values', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        const cr = await compileScript(c, USE_DEMO);
        assert.equal(cr.ok, true);

        // Run 5 times — verify run_count tracks correctly each time
        let prevCount = 0;
        for (let i = 1; i <= 5; ++i) {
            const { rsp, vars } = await runInspection(c);
            assert.equal(rsp.ok, true, `run ${i} ok`);
            const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));

            const count = byName.run_count.value;
            assert.equal(count, prevCount + 1,
                `run_count should increment monotonically: expected ${prevCount + 1}, got ${count}`);
            prevCount = count;

            // Also verify blob_count is a number (last_blob_count stored in state)
            assert.equal(typeof byName.blob_count.value, 'number', 'blob_count is a number');
        }
    });
});
