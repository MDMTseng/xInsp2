// ws_io_field.test.mjs — Field proxy: obj["x"] = obj["x"] * 0.533 + 45.
//
// operator[] returns a read/write proxy: reads via implicit conversion (so it
// drops into arithmetic), writes via operator= (write-through to the node).

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { writeFileSync, rmSync } from 'node:fs';
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
  auto e = synth_io::extract(syn.process(synth_io::build().seed(7).threshold(0.0).build()));
  auto c = e.centroid();              // a Point view (x, y)

  const double cx_orig = c["x"];      // read via implicit conversion
  VAR(orig, cx_orig);

  c["x"] = c["x"] * 0.533 + 45;       // read-modify-write
  VAR(x_new,    c["x"].as_double());
  VAR(expected, cx_orig * 0.533 + 45);

  // write-through: a fresh view of the same field sees it
  VAR(reread, e.centroid()["x"].as_double());

  const int as_int_ = (int)c["x"];    // int read
  VAR(int_read, as_int_);

  // Vec2 / Vec3 / Vec4 + operator[] arithmetic
  xi::Vec3 v(1, 2, 3);
  VAR(v_y, v.y());                    // 2
  v["z"] = v["z"] + 10;               // 13
  VAR(v_z, v["z"].as_double());
  xi::Vec4 q(0, 0, 0, 1);
  VAR(q_w, q.w());                    // 1
  xi::Vec2 p2(7, 8);
  VAR(p2_sum, (double)p2["x"] + (double)p2["y"]);  // 15
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('Field proxy read-modify-write: obj["x"] = obj["x"] * k + b', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (observed via vars)' }, async () => {
    const sp = join(PROJECT, '.field_probe.cpp');
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
            assert.ok(Math.abs(v.x_new - v.expected) < 1e-9, 'read-modify-write computed correctly');
            assert.ok(Math.abs(v.reread - v.x_new) < 1e-9, 'write-through visible on a fresh view');
            assert.equal(v.int_read, Math.trunc(v.x_new), 'int read truncates the value');
            assert.equal(v.v_y, 2, 'Vec3.y()');
            assert.equal(v.v_z, 13, 'Vec3 z via operator[] arithmetic');
            assert.equal(v.q_w, 1, 'Vec4.w()');
            assert.equal(v.p2_sum, 15, 'Vec2 components via operator[]');
        });
    } finally { try { rmSync(sp); } catch {} }
});
