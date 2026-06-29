// ws_na_propagation.test.mjs — Phase 1 of the typed-I/O design: NA backbone.
//
// Proves: xi::Record::na / is_na / na_reason round-trip through a run; feeding an
// NA Record into use("x").process() short-circuits to NA (plugin never runs) and
// carries the reason; xi::require() returns an NA naming the first missing field.
// See docs/internals/typed-io.md.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  auto na = xi::Record::na("boom");
  VAR(is_na,  na.is_na());
  VAR(reason, na.na_reason());

  // Short-circuit: process() on an NA input returns NA without running anything.
  auto out = xi::use("ghost").process(na);
  VAR(prop_na,     out.is_na());
  VAR(prop_reason, out.na_reason());

  // require(): names the first missing field.
  xi::Record partial; partial.set("current", 1);
  auto req = xi::require(partial, {"current", "baseline"});
  VAR(req_failed, (bool)req);
  VAR(req_reason, req ? req->na_reason() : std::string("ok"));
}`;

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }

test('NA: na/is_na/na_reason + process short-circuit + require', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (observed via vars)' }, async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_na_'));
    const sp = join(dir, 'inspect.cpp');
    writeFileSync(sp, SCRIPT);
    await withBackend(async (c) => {
        await c.nextText(); // hello
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(sp) } });
        assert.equal((await waitRsp(c, 1)).ok, true, 'compile ok');
        c.send({ type: 'cmd', id: 2, name: 'run' });
        await waitRsp(c, 2);
        let vars = null;
        for (;;) { const m = await c.nextText(); if (m.type === 'vars') { vars = m.items; break; } }
        const v = Object.fromEntries(vars.map(x => [x.name, x.value]));
        console.log('vars:', JSON.stringify(v));
        assert.equal(v.is_na, true, 'Record::na is_na');
        assert.equal(v.reason, 'boom', 'na_reason round-trips');
        assert.equal(v.prop_na, true, 'process(NA) short-circuits to NA');
        assert.equal(v.prop_reason, 'boom', 'NA reason propagates through process');
        assert.equal(v.req_failed, true, 'require() fails when a field is missing');
        assert.equal(v.req_reason, 'missing field: baseline', 'require names the missing field');
    });
});
