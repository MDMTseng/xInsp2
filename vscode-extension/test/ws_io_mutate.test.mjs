// ws_io_mutate.test.mjs — adjusting a value never touches the original.
//
// The shallow (view) wrappers share the source tree, so this nails down the
// safety contract: accessors are read-only, and the ways to "adjust" a value
// (record() copy, reconstruction) are independent of the original.

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withBackend } from './helpers/client.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const slash   = (s) => s.split('\\').join('/');
const PROJECT = resolve(__dirname, '..', '..', 'examples', 'io_stress');

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>
#include "plugins/synthetic_features/io.hpp"
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  auto syn = xi::use("syn");
  auto out = syn.process(synth_io::build().seed(7).threshold(0.0).build());
  auto e   = synth_io::extract(out);

  xi::Roi r = e.roi();              // a VIEW into out's tree
  VAR(orig_w, r.w());              // 640

  // Adjust via an independent copy (record() materialises).
  xi::Record edited = r.record();
  edited.set("w", 999.0);
  VAR(edited_w, edited["w"].as_double());

  // Original output + a fresh view are unchanged.
  VAR(out_roi_w_after, out["roi.w"].as_double());
  VAR(view_w_after,    e.roi().w());

  // Reconstruct a new value — also independent of the original.
  xi::Pose bp = e.best_pose();
  xi::Pose adjusted(bp.x(), bp.y(), 123.0);
  VAR(bp_angle,  bp.angle());
  VAR(adj_angle, adjusted.angle());
  VAR(bp_angle_after, e.best_pose().angle());
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('adjusting an extracted value does not affect the original', async () => {
    const { mkdtempSync, writeFileSync } = await import('node:fs');
    const { tmpdir } = await import('node:os');
    // need the project's plugin/instance, so compile the script inside it
    const sp = join(PROJECT, '.mutate_probe.cpp');
    writeFileSync(sp, SCRIPT);
    try {
        await withBackend(async (c) => {
            await c.nextText();
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJECT) } });
            assert.equal((await waitRsp(c, 1)).ok, true, 'open ok');
            c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(sp) } });
            assert.equal((await waitRsp(c, 2)).ok, true, 'compile ok');
            c.send({ type: 'cmd', id: 3, name: 'run' });
            await waitRsp(c, 3);
            let vars = null;
            for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
            const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
            console.log('vars:', JSON.stringify(v));
            assert.equal(v.orig_w, 640);
            assert.equal(v.edited_w, 999, 'the copy was edited');
            assert.equal(v.out_roi_w_after, 640, 'original output untouched');
            assert.equal(v.view_w_after, 640, 'a fresh view still reads the original');
            assert.equal(v.adj_angle, 123, 'reconstructed value has the new angle');
            assert.equal(v.bp_angle_after, v.bp_angle, 'original best_pose angle unchanged');
            assert.notEqual(v.adj_angle, v.bp_angle, 'adjustment is independent');
        });
    } finally {
        const { rmSync } = await import('node:fs');
        try { rmSync(sp); } catch {}
    }
});
