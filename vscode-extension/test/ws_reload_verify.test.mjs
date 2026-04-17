// Reload verification — proves that after hot-reload:
// 1. The NEW inspect function executes (not the old one)
// 2. Auxiliary thunks (params, state, reset) come from the new DLL
// 3. Old DLL code is completely unreachable

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { withBackend, compileScript, runInspection } from './helpers/client.mjs';

const tmpDir = resolve(tmpdir(), `xi_reload_verify_${Date.now()}`);
mkdirSync(tmpDir, { recursive: true });

// Two scripts with deliberately different outputs
const scriptA = resolve(tmpDir, 'version_a.cpp');
const scriptB = resolve(tmpDir, 'version_b.cpp');

writeFileSync(scriptA, `
#include <xi/xi.hpp>
xi::Param<int> marker{"version_marker", 111, {0, 999}};
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(version, std::string("ALPHA"));
    VAR(magic, 1111);
    VAR(marker_val, static_cast<int>(marker));
}
`);

writeFileSync(scriptB, `
#include <xi/xi.hpp>
xi::Param<int> new_param{"beta_param", 222, {0, 999}};
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    VAR(version, std::string("BRAVO"));
    VAR(magic, 2222);
    VAR(beta_val, static_cast<int>(new_param));
    VAR(frame_num, frame);
}
`);

test('after reload, inspect function is from the NEW DLL', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Compile and run version A
        const cr1 = await compileScript(c, scriptA);
        assert.equal(cr1.ok, true, 'version A compiles');

        const r1 = await runInspection(c);
        assert.equal(r1.vars.type, 'vars');
        const v1 = Object.fromEntries(r1.vars.items.map(v => [v.name, v]));

        assert.equal(v1.version.value, 'ALPHA', 'A: version is ALPHA');
        assert.equal(v1.magic.value, 1111, 'A: magic is 1111');
        assert.equal(v1.marker_val.value, 111, 'A: param default is 111');

        // Now compile and run version B — completely different script
        const cr2 = await compileScript(c, scriptB);
        assert.equal(cr2.ok, true, 'version B compiles');

        const r2 = await runInspection(c);
        assert.equal(r2.vars.type, 'vars');
        const v2 = Object.fromEntries(r2.vars.items.map(v => [v.name, v]));

        // These MUST be B's values, not A's
        assert.equal(v2.version.value, 'BRAVO', 'B: version is BRAVO (not ALPHA)');
        assert.equal(v2.magic.value, 2222, 'B: magic is 2222 (not 1111)');
        assert.equal(v2.beta_val.value, 222, 'B: new param default is 222');
        assert.equal(v2.frame_num.value, 1, 'B: frame_num exists (new var)');

        // A's vars should NOT appear in B's output
        assert.equal(v2.marker_val, undefined, 'A\'s marker_val gone in B');
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

test('after reload, set_param targets the NEW DLL param', async () => {
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

        // Run — beta_val should reflect the new value
        const r = await runInspection(c);
        const v = Object.fromEntries(r.vars.items.map(v => [v.name, v]));
        assert.equal(v.beta_val.value, 999, 'param update reaches NEW DLL');
    });
});

test('state() persists across A→B reload with different scripts', async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Write script C that writes to state
        const scriptC = resolve(tmpDir, 'state_writer.cpp');
        writeFileSync(scriptC, `
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    int count = xi::state()["reload_count"].as_int(0);
    xi::state().set("reload_count", count + 1);
    xi::state().set("last_version", "C");
    VAR(reload_count, count + 1);
    VAR(from, std::string("script_C"));
}
`);

        // Write script D that also reads/writes state
        const scriptD = resolve(tmpDir, 'state_reader.cpp');
        writeFileSync(scriptD, `
#include <xi/xi.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    int count = xi::state()["reload_count"].as_int(0);
    xi::state().set("reload_count", count + 1);
    xi::state().set("last_version", "D");
    VAR(reload_count, count + 1);
    VAR(from, std::string("script_D"));
    VAR(prev_version, xi::state()["last_version"].as_string("none"));
}
`);

        // Run C twice
        assert.equal((await compileScript(c, scriptC)).ok, true);
        await runInspection(c); // count 0→1
        await runInspection(c); // count 1→2

        let r = await runInspection(c); // count 2→3
        let v = Object.fromEntries(r.vars.items.map(v => [v.name, v]));
        assert.equal(v.reload_count.value, 3, 'C ran 3 times');

        // Reload to D — state should carry over
        assert.equal((await compileScript(c, scriptD)).ok, true);

        r = await runInspection(c); // count 3→4
        v = Object.fromEntries(r.vars.items.map(v => [v.name, v]));
        assert.equal(v.reload_count.value, 4, 'D continues from C\'s state (3→4)');
        assert.equal(v.from.value, 'script_D', 'running D not C');
    });
});

test('reset thunk is from new DLL (ValueStore clears properly)', async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // A produces 3 vars
        assert.equal((await compileScript(c, scriptA)).ok, true);
        const r1 = await runInspection(c);
        assert.equal(r1.vars.items.length, 3, 'A has 3 vars');

        // B produces 4 vars — if reset didn't work, we'd see 3+4=7
        assert.equal((await compileScript(c, scriptB)).ok, true);
        const r2 = await runInspection(c);
        assert.equal(r2.vars.items.length, 4, 'B has exactly 4 vars (not 3+4=7)');
    });
});
