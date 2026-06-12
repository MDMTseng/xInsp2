// ws_io_stress.test.mjs — extractor/constructor against a RICH fake contract.
//
// Opens examples/io_stress (synthetic_features, pure fake data) and checks the
// wiring layer handles nested records, a typed array, a custom nominal type
// (Feature), provenance, and NA — all over a schema-less Record.

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withBackend } from './helpers/client.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const slash   = (s) => s.split('\\').join('/');
const PROJECT = resolve(__dirname, '..', '..', 'examples', 'io_stress');

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('io_stress: rich extract/construct (nested, array, custom type, NA, provenance)', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJECT) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'open_project (compiles the plugin) ok');
        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(PROJECT, 'inspect.cpp')) } });
        assert.equal((await waitRsp(c, 2)).ok, true, 'script compiled (includes io.hpp)');
        c.send({ type: 'cmd', id: 3, name: 'run' });
        await waitRsp(c, 3);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));

        // seed 7 → n = 7%5+3 = 5 raw features; scores ((49 + i*13)%100)/100 =
        // 0.49, 0.62, 0.75, 0.88, 0.01 ; threshold 0.30 keeps the first four.
        assert.equal(v.count, 4, 'threshold filtered to 4 features');
        assert.equal(v.feat_count, 4, 'features array length');
        assert.equal(v.vec_size, 4, 'typed vector<Feature>');

        // Nested + custom-type extractors return real values.
        assert.equal(typeof v.centroid_x, 'number');
        assert.equal(typeof v.best_x, 'number');
        assert.ok(v.best_score >= 0.30, 'best_score is the max kept score');
        assert.equal(v.roi_w, 640, 'roi echoed/defaulted');
        assert.equal(typeof v.mean_score, 'number');
        assert.equal(typeof v.f0_pose_x, 'number', 'Feature.pose().x()');
        assert.ok(Math.abs(v.f0_edge_x2 - (v.f0_pose_x + 20)) < 1e-6, 'Feature.edge().x2() == pose.x + 20');

        // Provenance piped through extractor.
        assert.equal(v.out_src, 'syn', 'output stamped with instance src');
        assert.equal(v.f0_src, 'syn', 'extractor piped src onto the Feature');

        // Input constructor with a TYPED value (extract → construct): the Roi was
        // fed into the next run's constructor, which recorded its source, and it
        // round-trips back out.
        assert.equal(v.in2_roi_prov, 'syn', 'constructor recorded the ROI came from "syn"');
        assert.equal(v.out2_roi_w, 640, 'the typed ROI fed into build().roi() round-trips');
        assert.equal(typeof v.out2_count, 'number', 'second run produced its own output');

        // NA path — require fires, extractors stay total.
        assert.equal(v.na_is_na, true, 'no seed → NA');
        assert.match(v.na_reason, /seed/, 'NA names the missing field');
        assert.equal(v.na_count, 0, 'extract(NA).count() is 0, not a crash');
        assert.equal(v.na_feat_count, 0);
        assert.equal(v.na_feature_na, true, 'typed NA propagates into Feature');
    });
});
