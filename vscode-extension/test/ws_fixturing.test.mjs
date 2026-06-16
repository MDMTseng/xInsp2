// ws_fixturing.test.mjs — Phase 3: the whole typed-I/O story end to end.
//
// Opens examples/fixturing_demo (locator + line_fit, both compiled from source),
// runs it, and asserts:
//   - the extractor adapted the locator's raw centroids into typed Poses,
//   - the constructor wired pose 0 into line_fit and the pose-aligned Line came
//     back transformed (baseline line shifted to the pose, angle 0),
//   - feeding line_fit no current pose makes it return NA (require), and that NA
//     propagates into the typed Line — with no defensive code in the wiring.
// See docs/internals/typed-io.md.

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withBackend } from './helpers/client.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const slash   = (s) => s.split('\\').join('/');
const PROJECT = resolve(__dirname, '..', '..', 'examples', 'fixturing_demo');
const FRAME   = slash(join(PROJECT, 'frames', 'frame_00.png'));

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('fixturing: extractor → constructor → plugin validate/NA, end to end', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJECT) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'open_project (compiles both plugins) ok');
        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(PROJECT, 'inspect.cpp')) } });
        assert.equal((await waitRsp(c, 2)).ok, true, 'script compiled (includes both io.hpp)');
        c.send({ type: 'cmd', id: 3, name: 'run', args: { frame_path: FRAME } });
        await waitRsp(c, 3);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));

        // Extractor produced typed poses from the locator's raw centroids.
        assert.ok(v.n_poses >= 1, 'locator found at least one part');
        assert.equal(typeof v.pose0_x, 'number', 'pose 0 extracted');

        // Constructor → line_fit → typed Line, transformed to the pose. With the
        // default baseline (0,0,0) + line (10,..)-(10,..) and angle 0, the
        // transform is a pure translation: line_x1 == pose0_x + 10.
        assert.equal(v.fit_na, false, 'fit ran (current pose present)');
        assert.ok(Math.abs(v.line_x1 - (v.pose0_x + 10)) < 1e-6, 'line transformed to the pose');

        // NA propagation: no current pose → require → NA → into the typed Line.
        assert.equal(v.na_out_na, true, 'line_fit returns NA when current is missing');
        assert.match(v.na_reason, /current/, 'NA names the missing field');
        assert.equal(v.na_line_na, true, 'NA propagated into the typed Line wrapper');

        // Provenance (src id): host stamps the output, the extractor pipes it
        // onto the pose, the constructor records the field's source.
        assert.equal(v.loc_src, 'loc', 'host stamped the locator output with $src');
        assert.equal(v.pose0_src, 'loc', 'extractor piped src onto the pose');
        assert.equal(v.in_prov_current, 'loc', 'constructor recorded current came from loc');
    });
});
