// Reload verification — proves that after hot-reload:
// 1. The NEW inspect function executes (not the old one)
// 2. Auxiliary thunks (params, kv state) come from the new DLL
// 3. Old DLL code is completely unreachable
//
// v12 (THE CUT): the vars frame + VAR are gone — each script surfaces its
// identity through the per-run verdict (xi::result → run_result event), and
// cross-frame state is xi::kv() (xi::state() was deleted, docs/new_gen/16).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { withBackend, compileScript } from './helpers/client.mjs';

const tmpDir = resolve(tmpdir(), `xi_reload_verify_${Date.now()}`);
mkdirSync(tmpDir, { recursive: true });

// Two scripts with deliberately different verdicts
const scriptA = resolve(tmpDir, 'version_a.cpp');
const scriptB = resolve(tmpDir, 'version_b.cpp');

// A: code = 1000 + marker (default 111 → 1111), msg "ALPHA"
writeFileSync(scriptA, `
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
xi::Param<int> marker{"version_marker", 111, {0, 999}};
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::result(1000 + (int)marker, "ALPHA");
}
`);

// B: code = 2000 + beta_param (default 222 → 2222), msg "BRAVO"
writeFileSync(scriptB, `
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
xi::Param<int> new_param{"beta_param", 222, {0, 999}};
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::result(2000 + (int)new_param, "BRAVO");
}
`);

// Run once, return the run_result event data.
async function runResult(c, id) {
    c.send({ type: 'cmd', id, name: 'run' });
    for (;;) {
        const m = await c.nextText(120000);
        if (m.type === 'event' && m.name === 'run_result') return m.data;
    }
}

test('after reload, inspect function is from the NEW DLL', { timeout: 120000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Compile and run version A
        const cr1 = await compileScript(c, scriptA);
        assert.equal(cr1.ok, true, 'version A compiles');

        const r1 = await runResult(c, 10);
        assert.equal(r1.msg, 'ALPHA', 'A: msg is ALPHA');
        assert.equal(r1.code, 1111, 'A: code carries marker default (1000+111)');

        // Now compile and run version B — completely different script
        const cr2 = await compileScript(c, scriptB);
        assert.equal(cr2.ok, true, 'version B compiles');

        const r2 = await runResult(c, 11);
        // These MUST be B's values, not A's
        assert.equal(r2.msg, 'BRAVO', 'B: msg is BRAVO (not ALPHA)');
        assert.equal(r2.code, 2222, 'B: code carries beta default (2000+222), not A\'s 1111');
    });
});

test('after reload, param registry is from the NEW DLL', async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Load A
        const cr1 = await compileScript(c, scriptA);
        assert.equal(cr1.ok, true);

        // List params — should have version_marker from A
        c.send({ type: 'cmd', id: 10, name: 'list_params' });
        const rsp1 = await c.nextText(10000); // rsp
        const inst1 = await c.nextText(10000); // instances msg
        const paramNames1 = (inst1.params || []).map(p => p.name);
        assert.ok(paramNames1.includes('version_marker'), 'A: has version_marker param');

        // Reload B
        const cr2 = await compileScript(c, scriptB);
        assert.equal(cr2.ok, true);

        // List params — should have beta_param from B, NOT version_marker
        c.send({ type: 'cmd', id: 11, name: 'list_params' });
        const rsp2 = await c.nextText(10000);
        const inst2 = await c.nextText(10000);
        const paramNames2 = (inst2.params || []).map(p => p.name);
        assert.ok(paramNames2.includes('beta_param'), 'B: has beta_param');
        assert.ok(!paramNames2.includes('version_marker'), 'B: version_marker gone');
    });
});

test('after reload, set_param targets the NEW DLL param', { timeout: 120000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Load B
        const cr = await compileScript(c, scriptB);
        assert.equal(cr.ok, true);

        // Set B's param to a custom value
        c.send({ type: 'cmd', id: 10, name: 'set_param',
                 args: { name: 'beta_param', value: 999 } });
        const sr = await c.nextText(10000);
        assert.equal(sr.ok, true, 'set_param ok');

        // Run — the verdict should reflect the new value (2000+999)
        const r = await runResult(c, 11);
        assert.equal(r.code, 2999, 'param update reaches NEW DLL');
    });
});

test('xi::kv() state persists across A→B reload with different scripts', { timeout: 120000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Script C writes to kv (schema 1)
        const scriptC = resolve(tmpDir, 'state_writer.cpp');
        writeFileSync(scriptC, `
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
XI_KV_SCHEMA(1);
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    long long count = xi::kv().get_i64("reload_count", 0) + 1;
    xi::kv().set_i64("reload_count", count);
    xi::kv().set_str("last_version", "C");
    xi::result((int)count, "script_C");
}
`);

        // Script D also reads/writes kv (same schema → host restores, no drop)
        const scriptD = resolve(tmpDir, 'state_reader.cpp');
        writeFileSync(scriptD, `
#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
XI_KV_SCHEMA(1);
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    std::string prev = xi::kv().get_str("last_version", "none");
    long long count = xi::kv().get_i64("reload_count", 0) + 1;
    xi::kv().set_i64("reload_count", count);
    xi::kv().set_str("last_version", "D");
    xi::result((int)count, "script_D:prev=" + prev);
}
`);

        // Run C three times
        assert.equal((await compileScript(c, scriptC)).ok, true);
        await runResult(c, 20); // count 0→1
        await runResult(c, 21); // count 1→2
        const r3 = await runResult(c, 22); // count 2→3
        assert.equal(r3.code, 3, 'C ran 3 times');
        assert.equal(r3.msg, 'script_C');

        // Reload to D — kv state should carry over (capture off old DLL,
        // restore into new; same XI_KV_SCHEMA so no state_dropped)
        assert.equal((await compileScript(c, scriptD)).ok, true);

        const r4 = await runResult(c, 23); // count 3→4
        assert.equal(r4.code, 4, 'D continues from C\'s kv state (3→4)');
        assert.equal(r4.msg, 'script_D:prev=C',
            'D read C\'s last_version out of the restored kv store');
    });
});

// ("reset thunk is from new DLL (ValueStore clears properly)" was removed:
// the VAR/ValueStore surface it observed no longer exists — the per-run
// verdict + param-registry tests above cover the new-DLL-thunks property.)
