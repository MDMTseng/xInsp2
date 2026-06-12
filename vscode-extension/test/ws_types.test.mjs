// ws_types.test.mjs — Phase 2: nominal types (names over Record).
//
// A nominal type is just a name + a lightweight handle over a schema-less
// Record: field accessors read conventional keys, it can be NA, and it round-
// trips through Record (what crosses into a constructor / process()).
// See docs/design/io-types-and-na.md.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_types.hpp>
#include <vector>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  xi::Pose p(10, 20, 45);
  VAR(px,    p.x());
  VAR(angle, p.angle());
  VAR(p_na,  p.is_na());

  auto bad = xi::Pose::na("no match");
  VAR(bad_na,     bad.is_na());
  VAR(bad_reason, bad.na_reason());
  VAR(bad_angle,  bad.angle());        // NA → default 0

  std::vector<xi::Pose> v{ xi::Pose(1,1,0), xi::Pose(2,2,90) };
  VAR(vcount,  (int)v.size());
  VAR(v1angle, v[1].angle());

  xi::Record r = p.record();           // round-trips through the payload
  VAR(r_x, r["x"].as_double(0));

  xi::Line  ln(0,0,100,0);  VAR(line_x2, ln.x2());
  xi::Number n(3.5);        VAR(num_val, n.value());
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('nominal types: accessors, NA, vector handle, Record round-trip', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_types_'));
    const sp = join(dir, 'inspect.cpp');
    writeFileSync(sp, SCRIPT);
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(sp) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'compile ok');
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await waitRsp(c, 2);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));
        assert.equal(v.px, 10);
        assert.equal(v.angle, 45);
        assert.equal(v.p_na, false);
        assert.equal(v.bad_na, true);
        assert.equal(v.bad_reason, 'no match');
        assert.equal(v.bad_angle, 0, 'NA pose accessor → default');
        assert.equal(v.vcount, 2);
        assert.equal(v.v1angle, 90, 'vector<Pose> handle works');
        assert.equal(v.r_x, 10, 'round-trips through Record payload');
        assert.equal(v.line_x2, 100);
        assert.equal(v.num_val, 3.5);
    });
});
