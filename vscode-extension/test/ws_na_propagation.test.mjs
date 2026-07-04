// ws_na_propagation.test.mjs — the pack-plane error path ($fault, doc 15).
//
// v12 port of the Record NA-backbone test. Proves, end to end:
//   1. a script-built FAULT pack (ScriptPackBuilder::fault) reads back via
//      is_fault()/fault_reason() — a fault is a NORMAL sealed pack carrying
//      "$fault", never a crash;
//   2. feeding a fault pack into use(x).process() SHORT-CIRCUITS host-side:
//      the plugin never runs and the result is a NEW fault pack carrying the
//      original reason with this hop stamped ($src) — the pack mirror of the
//      retired Record NA propagation;
//   3. a CONTRACT failure inside the door (missing input key) comes back as a
//      normal sealed pack with $fault=missing_input + $fault_key naming the
//      offending key (fail-loud, not an empty result);
//   4. a clean call still works (result = value * 2).
//
// Observed via the per-run verdict (xi::result → run_result): code carries the
// clean call's result, msg carries the fault-path observations.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const PLUGIN_CPP = `// dbl — doubles i64 "value"; missing input is a $fault pack (doc 15).
class Dbl : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    void process(xi::PackIn& in, xi::PackOut& out) override {
        auto v = in.i64("value");
        if (!v) { out.fault("missing_input", "value", "dbl needs i64 'value'"); return; }
        out.i64("result", *v * 2).i64("ran", 1);
    }
};
XI_PLUGIN_IMPL(Dbl)
XI_PLUGIN_PACK_DOOR(Dbl)
`;

const PLUGIN_JSON = JSON.stringify({
    name: 'dbl_probe', description: 'fault-path probe (doubles value)',
    dll: 'dbl_probe.dll', factory: 'xi_plugin_create', has_ui: false,
}, null, 2);

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  std::string msg;

  // 1. Script-built fault pack: a normal sealed pack carrying "$fault".
  xi::ScriptPackBuilder fb;
  fb.fault("boom");
  auto na = fb.seal();
  msg += na.is_fault() ? "fault" : "clean";
  msg += "|" + std::string(na.fault_reason().value_or("none"));

  // 2. Short-circuit: process() on a fault input propagates WITHOUT running
  //    the plugin (host mints a new fault pack; $src = this hop).
  auto out = xi::use("dbl").process(na);
  msg += "|" + std::string(out.is_fault() ? "prop" : "notprop");
  msg += "|" + std::string(out.fault_reason().value_or("none"));
  msg += "|src=" + std::string(out.src().value_or("none"));
  msg += "|ran=" + std::to_string(out.get_i64("ran").value_or(0));

  // 3. Contract failure: door returns a $fault pack naming the missing key.
  xi::ScriptPackBuilder eb;
  eb.add_i64("other", 1);
  auto r2 = xi::use("dbl").process(eb.seal());
  msg += "|" + std::string(r2.fault_reason().value_or("none"));
  msg += "|key=" + std::string(r2.fault_key().value_or("none"));

  // 4. Clean call: the verdict code is the doubled value.
  xi::ScriptPackBuilder gb;
  gb.add_i64("value", 21);
  auto r3 = xi::use("dbl").process(gb.seal());
  xi::result((int)r3.get_i64("result").value_or(-1), msg);
}`;

function makeProject(dir) {
    mkdirSync(join(dir, 'plugins', 'dbl_probe', 'src'), { recursive: true });
    mkdirSync(join(dir, 'instances', 'dbl'), { recursive: true });
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'fault_path', script: 'inspect.cpp', params: [], instances: [],
            plugins: { dbl_probe: { path: 'dbl_probe', compile: true } } }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'), SCRIPT);
    writeFileSync(join(dir, 'plugins', 'dbl_probe', 'plugin.json'), PLUGIN_JSON);
    writeFileSync(join(dir, 'plugins', 'dbl_probe', 'src', 'plugin.cpp'), PLUGIN_CPP);
    writeFileSync(join(dir, 'instances', 'dbl', 'instance.json'),
        JSON.stringify({ plugin: 'dbl_probe', config: {} }, null, 2));
}

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(150000); if (m.type === 'rsp' && m.id === id) return m; } }

test('$fault: build/read + process short-circuit + contract fail-loud (doc 15)', { timeout: 180000 }, async () => {
    const dir = resolve(tmpdir(), `xi_fault_${Date.now()}`);
    makeProject(dir);
    try {
        await withBackend(async (c) => {
            await c.nextText(); // hello
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
            assert.equal((await waitRsp(c, 1)).ok, true, 'open_project ok');
            c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
            assert.equal((await waitRsp(c, 2)).ok, true, 'compile ok');

            c.send({ type: 'cmd', id: 3, name: 'run' });
            let result = null;
            for (;;) {
                const m = await c.nextText(150000);
                if (m.type === 'event' && m.name === 'run_result') { result = m.data; break; }
            }
            console.log('fault-path run_result:', JSON.stringify(result));
            const parts = (result.msg || '').split('|');

            assert.equal(parts[0], 'fault', 'ScriptPackBuilder::fault seals a fault pack');
            assert.equal(parts[1], 'boom', 'fault_reason round-trips');
            assert.equal(parts[2], 'prop', 'process(fault) short-circuits to a fault');
            assert.equal(parts[3], 'boom', 'original reason propagates through process');
            assert.equal(parts[4], 'src=dbl', 'the propagated fault is stamped with this hop as $src');
            assert.equal(parts[5], 'ran=0', 'the plugin never ran on a fault input');
            assert.equal(parts[6], 'missing_input', 'contract failure comes back as $fault=missing_input');
            assert.equal(parts[7], 'key=value', '$fault_key names the missing input key');
            assert.equal(result.code, 42, 'clean call still works (21 * 2)');
        });
    } finally {
        try { rmSync(dir, { recursive: true }); } catch {}
    }
});
