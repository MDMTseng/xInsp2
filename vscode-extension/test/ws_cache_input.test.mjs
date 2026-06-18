// ws_cache_input.test.mjs — γ-4 v4-4 end-to-end: a real plugin CACHES its input
// doc across frames (zero-copy via the cross-ABI refcount) and reads it back the
// next frame. If the cached input were a borrowed/dangling pointer (the pre-v4-4
// hazard) this would read garbage or crash; with v4-4 the registry keeps the doc
// alive as long as the plugin holds it.
//
// Builds a throwaway project with a cache_probe plugin + a script that feeds it
// an incrementing seq each run, then runs 3 frames and checks that frame N sees
// frame N-1's input as its "prev".

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const slash = (s) => s.split('\\').join('/');

const PLUGIN_CPP = `#include <xi/xi.hpp>
// Caches its previous input across frames. The cache is a plain Record copy —
// under γ-4 that's a refcount bump on the host-owned input doc (zero deep copy),
// and the registry keeps the doc alive until this plugin drops it next frame.
class CacheProbe : public xi::Plugin {
    xi::Record prev_;   // cached PREVIOUS input
public:
    using xi::Plugin::Plugin;
    xi::Record process(const xi::Record& in) override {
        int cur  = in.get_int("seq", -1);
        int prev = prev_.get_int("seq", -999);  // read last frame's cached input
        prev_ = in;                             // cache THIS input for next frame
        return xi::Record().set("cur", cur).set("prev", prev);
    }
};
XI_PLUGIN_IMPL(CacheProbe)
`;

const PLUGIN_JSON = JSON.stringify({
    name: 'cache_probe',
    description: 'caches its input across frames (γ-4 v4-4 test)',
    dll: 'cache_probe.dll',
    factory: 'xi_plugin_create',
    has_ui: false,
}, null, 2);

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
static int g_seq = 0;
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  auto probe = xi::use("probe");
  auto out = probe.process(xi::Record().set("seq", g_seq++));
  VAR(cur,  out["cur"].as_int(-1));
  VAR(prev, out["prev"].as_int(-1));
}`;

function makeProject(dir) {
    mkdirSync(join(dir, 'plugins', 'cache_probe', 'src'), { recursive: true });
    mkdirSync(join(dir, 'instances', 'probe'), { recursive: true });
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'cache_test', script: 'inspect.cpp', params: [], instances: [],
            plugins: { cache_probe: { path: 'cache_probe', compile: true } } }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'), SCRIPT);
    writeFileSync(join(dir, 'plugins', 'cache_probe', 'plugin.json'), PLUGIN_JSON);
    writeFileSync(join(dir, 'plugins', 'cache_probe', 'src', 'plugin.cpp'), PLUGIN_CPP);
    writeFileSync(join(dir, 'instances', 'probe', 'instance.json'),
        JSON.stringify({ plugin: 'cache_probe', config: {} }, null, 2));
}

async function waitRsp(c, id) { for (;;) { const m = await c.nextText(150000); if (m.type === 'rsp' && m.id === id) return m; } }

async function runFrame(c, id) {
    c.send({ type: 'cmd', id, name: 'run' });
    let rspOk = false, vars = null;
    for (;;) {
        const m = await c.nextText(150000);
        if (m.type === 'rsp' && m.id === id) rspOk = (m.ok !== false);
        if (m.type === 'vars') { vars = Object.fromEntries(m.items.map(x => [x.name, x.value])); }
        if (rspOk && vars) break;
    }
    return vars;
}

test('plugin caches its input across frames (γ-4 v4-4 zero-copy, end-to-end)', { timeout: 180000 }, async () => {
    const dir = resolve(tmpdir(), `xinsp2_cacheprobe_${Date.now()}`);
    makeProject(dir);
    try {
        await withBackend(async (c) => {
            await c.nextText(); // hello
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
            assert.equal((await waitRsp(c, 1)).ok, true, 'open_project ok');
            c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
            assert.equal((await waitRsp(c, 2)).ok, true, 'compile_and_load ok');

            const f0 = await runFrame(c, 10);
            const f1 = await runFrame(c, 11);
            const f2 = await runFrame(c, 12);
            console.log('frames:', JSON.stringify([f0, f1, f2]));

            // cur counts up per frame; prev is the PREVIOUS frame's cached input.
            assert.equal(f0.cur, 0, 'frame 0 cur');
            assert.equal(f0.prev, -999, 'frame 0 has no cached input yet');
            assert.equal(f1.cur, 1, 'frame 1 cur');
            assert.equal(f1.prev, 0, 'frame 1 sees frame 0 cached input');
            assert.equal(f2.cur, 2, 'frame 2 cur');
            assert.equal(f2.prev, 1, 'frame 2 sees frame 1 cached input');
        });
    } finally {
        try { rmSync(dir, { recursive: true }); } catch {}
    }
});
