// ws_io_mutate.test.mjs — the view write-through / clone contract.
//
// Extracted values are SHALLOW views: set() writes through to the original by
// design; .clone() first if you want an independent copy. See
// docs/design/io-types-and-na.md.

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
  // The extractor OWNS the process result (moved in). Its views write through to
  // that owned record — which is what flows downstream.
  auto e = synth_io::extract(syn.process(synth_io::build().seed(7).threshold(0.0).build()));
  VAR(before, e.roi().w());          // 640

  // WRITE-THROUGH: set() on a view mutates the extractor's record.
  e.roi().set("w", 999.0);
  VAR(after_write, e.roi().w());     // 999 — a fresh view sees it

  // ...and the mutation flows into a constructed input.
  auto in = synth_io::build().roi(e.roi()).build();
  VAR(fed_w, in["roi.w"].as_double());  // 999

  // CLONE: an independent copy — editing it does NOT touch the extractor's record.
  e.roi().clone().set("w", 111.0);
  VAR(after_clone, e.roi().w());     // still 999

  // record() likewise gives an independent copy.
  xi::Record c = e.roi().record();
  c.set("w", 222.0);
  VAR(after_record_copy, e.roi().w()); // still 999
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
            assert.equal(v.before, 640);
            assert.equal(v.after_write, 999, 'set() on a view writes through to the extractor record');
            assert.equal(v.fed_w, 999, 'the write-through flows into a constructed input');
            assert.equal(v.after_clone, 999, 'editing a clone does NOT touch the extractor record');
            assert.equal(v.after_record_copy, 999, 'editing a record() copy does NOT touch it either');
        });
    } finally {
        const { rmSync } = await import('node:fs');
        try { rmSync(sp); } catch {}
    }
});
