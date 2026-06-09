// ws_multi_file_script.test.mjs — proves the multi-file-via-headers script model.
//
// A script is ONE translation unit. The supported way to split a complex
// script across files is to put each lane / block in its own HEADER and
// #include it into inspect.cpp. This test compiles such a project and runs it,
// asserting that VAR()s declared INSIDE the included headers surface — i.e. the
// headers really are part of the script's TU (and, by the same token, xi::use()
// in a header links: the fixture's lane_a.hpp calls it under a never-taken
// branch, so a successful compile is also a link-proof).
//
// This is the counter-evidence to "just add a sources[] array": separate .cpp
// TUs can't see the per-TU script-support thunks / xi::use() globals, so they'd
// fail to link. Headers don't, because they compile into this TU.

import test from 'node:test';
import assert from 'node:assert/strict';
import { withBackend, scriptPath } from './helpers/client.mjs';

const inspect = scriptPath('multi_file_script/inspect.cpp');

test('multi-file script (headers #included into one TU) compiles + surfaces lane VARs', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        // Compile inspect.cpp — it #includes lanes/lane_a.hpp + lanes/lane_b.hpp.
        // A clean compile proves both that the headers are found (cl resolves
        // "lanes/..." relative to inspect.cpp) AND that xi::use() inside a header
        // links (lane_a.hpp references it).
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: inspect } });
        const cr = await c.nextNonLog();
        assert.equal(cr.ok, true, 'multi-file script must compile + link: ' + JSON.stringify(cr.error ?? ''));

        // Run and collect the surfaced variables.
        c.send({ type: 'cmd', id: 2, name: 'run' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'run ok');
        const vars = await c.nextNonLog();
        assert.equal(vars.type, 'vars');

        const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));
        // VARs declared in lane_a.hpp ...
        assert.equal(byName.lane_a_tag?.kind, 'string', 'lane_a_tag present');
        assert.equal(byName.lane_a_tag?.value, 'alpha');
        assert.ok('lane_a_frame' in byName, 'lane_a_frame present');
        // ... and in lane_b.hpp — both lanes live in their own files yet both
        // surface, proving they compiled into the one script TU.
        assert.equal(byName.lane_b_tag?.kind, 'string', 'lane_b_tag present');
        assert.equal(byName.lane_b_tag?.value, 'beta');
    });
});
