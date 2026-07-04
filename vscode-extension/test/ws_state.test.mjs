// ws_state.test.mjs — cross-frame script state (xi::kv(), docs/new_gen/16)
// persists across runs, hot-reloads and param changes.
//
// v12 (THE CUT): xi::state() (the Record/JSON state channel) is deleted;
// xi::kv() is the ONE cross-frame store. The vars frame is gone too, so the
// script surfaces the counter through the per-run verdict (xi::result → the
// run_result event): code = the incremented run_count.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { withBackend, compileScript } from './helpers/client.mjs';

const tmpDir = resolve(tmpdir(), `xi_kv_state_${Date.now()}`);
mkdirSync(tmpDir, { recursive: true });

// Counts runs in xi::kv()["run_count"]; the count IS the verdict code.
const KV_SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
XI_KV_SCHEMA(1);
xi::Param<int> threshold{"threshold", 128, {0, 255}};
XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    long long n = xi::kv().get_i64("run_count", 0) + 1;
    xi::kv().set_i64("run_count", n);
    xi::result((int)n, "kv_carry");
}
`;
const scriptFile = resolve(tmpDir, 'kv_state.cpp').split('\\').join('/');
writeFileSync(scriptFile, KV_SCRIPT);

// Run once and return the run_result event's data.
async function runResult(c, id) {
    c.send({ type: 'cmd', id, name: 'run' });
    for (;;) {
        const m = await c.nextText(120000);
        if (m.type === 'event' && m.name === 'run_result') return m.data;
    }
}

// ---------------------------------------------------------------
// 1. kv state persists across runs — run_count increments each time
// ---------------------------------------------------------------
test('kv state persists across runs: run_count increments', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        const cr = await compileScript(c, scriptFile);
        assert.equal(cr.ok, true, 'compile ok');

        // Run 3 times and verify run_count increments
        for (let expected = 1; expected <= 3; ++expected) {
            const r = await runResult(c, 100 + expected);
            assert.equal(r.code, expected,
                `run_count should be ${expected}, got ${r.code}`);
        }
    });
});

// ---------------------------------------------------------------
// 2. kv state survives hot-reload (recompile same script, same schema)
// ---------------------------------------------------------------
test('kv state survives hot-reload: recompile does not reset run_count', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Compile and run once → run_count = 1
        const cr1 = await compileScript(c, scriptFile);
        assert.equal(cr1.ok, true);
        assert.equal((await runResult(c, 110)).code, 1);

        // Recompile the same script (hot-reload). The host captures the kv
        // bytes off the OLD DLL and restores them into the NEW one (same
        // XI_KV_SCHEMA → no state_dropped).
        const cr2 = await compileScript(c, scriptFile);
        assert.equal(cr2.ok, true, 'recompile ok');

        // Run again → run_count should be 2, NOT reset to 1
        assert.equal((await runResult(c, 111)).code, 2,
            'run_count should be 2 after recompile, kv state must survive hot-reload');
    });
});

// ---------------------------------------------------------------
// 3. kv state survives param change
// ---------------------------------------------------------------
test('kv state survives param change: set_param does not clear state', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        const cr = await compileScript(c, scriptFile);
        assert.equal(cr.ok, true);

        // Run once → run_count = 1
        assert.equal((await runResult(c, 120)).code, 1);

        // Change a param
        c.send({ type: 'cmd', id: 10, name: 'set_param',
                 args: { name: 'threshold', value: 200 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true, 'set_param ok');

        // Run again → run_count should be 2 (state not cleared by param change)
        assert.equal((await runResult(c, 121)).code, 2,
            'run_count should be 2 after param change, kv state must survive');
    });
});

// ---------------------------------------------------------------
// 4. kv state carries monotonically over many runs
// ---------------------------------------------------------------
test('kv state carries monotonically across multiple runs', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        const cr = await compileScript(c, scriptFile);
        assert.equal(cr.ok, true);

        let prev = 0;
        for (let i = 1; i <= 5; ++i) {
            const r = await runResult(c, 130 + i);
            assert.equal(r.code, prev + 1,
                `run_count should increment monotonically: expected ${prev + 1}, got ${r.code}`);
            prev = r.code;
        }
    });
});
