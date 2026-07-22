// ws_cache_input.test.mjs — pack-plane input caching end-to-end: a real plugin
// CACHES its input PACK across frames (a refcount it takes through the
// xi.pack@1 vtable — zero copy) and reads it back the next frame. If the
// cached input were a borrowed/dangling handle this would read garbage or
// crash; the host pack registry keeps the sealed pack (and its pool buffers)
// alive as long as the plugin holds its ref.
//
// v12 port of the γ-4 Record-doc cache test: the plugin's data plane is the
// pack door (process(PackIn&, PackOut&) + XI_PLUGIN_PACK_DOOR), the script
// builds its input with xi::ScriptPackBuilder, and the observable rides the
// per-run verdict (xi::result → run_result) since the vars frame is gone.
//
// Builds a throwaway project with a cache_probe plugin + a script that feeds it
// an incrementing seq each run, then runs 3 frames and checks that frame N sees
// frame N-1's input as its "prev".

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, join } from 'node:path';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const PLUGIN_CPP = `// cache_probe — caches its previous input PACK across frames.
// The cache is an owned refcount on the sealed input handle (taken through the
// process-stable xi.pack@1 vtable); the host registry keeps the pack alive
// until this plugin drops it next frame.
class CacheProbe : public xi::Plugin {
    xi_pack_handle prev_ = XI_PACK_NULL;   // OWNED ref on the previous input
public:
    using xi::Plugin::Plugin;
    ~CacheProbe() override {
        const xi_pack_v1* fi = pack_iface();
        if (prev_ != XI_PACK_NULL && fi && fi->release) fi->release(prev_);
    }
    void process(xi::PackIn& in, xi::PackOut& out) override {
        const xi_pack_v1* fi = pack_iface();
        long long cur  = in.i64_or("seq", -1);
        long long prev = -999;
        if (prev_ != XI_PACK_NULL && fi) {
            xi::PackIn prev_in(fi, prev_);     // read LAST frame's cached input
            prev = prev_in.i64_or("seq", -999);
        }
        if (fi && fi->retain && fi->release) { // cache THIS input for next frame
            fi->retain(in.handle());
            if (prev_ != XI_PACK_NULL) fi->release(prev_);
            prev_ = in.handle();
        }
        out.i64("cur", cur).i64("prev", prev);
    }
};
XI_PLUGIN_IMPL(CacheProbe)
XI_PLUGIN_PACK_DOOR(CacheProbe)
`;

const PLUGIN_JSON = JSON.stringify({
    name: 'cache_probe',
    description: 'caches its input pack across frames (pack-plane cache test)',
    dll: 'cache_probe.dll',
    factory: 'xi_plugin_create',
    has_ui: false,
}, null, 2);

// code = cur+1 (cur starts at 0; 0 would read as the NA verdict band),
// msg = "prev=<prev>".
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>
static int g_seq = 0;
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  xi::ScriptPackBuilder b;
  b.add_i64("seq", g_seq++);
  auto out = xi::use("probe").process(b.seal());
  long long cur  = out.get_i64("cur").value_or(-1);
  long long prev = out.get_i64("prev").value_or(-1);
  xi::result((int)cur + 1, "prev=" + std::to_string(prev));
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

// Run one frame; return { cur, prev } decoded from the run_result verdict.
async function runFrame(c, id) {
    c.send({ type: 'cmd', id, name: 'run' });
    for (;;) {
        const m = await c.nextText(150000);
        if (m.type === 'event' && m.name === 'run_result') {
            const prev = Number((/prev=(-?\d+)/.exec(m.data.msg || '') || [])[1]);
            return { cur: m.data.code - 1, prev };
        }
    }
}

test('plugin caches its input pack across frames (registry keepalive, end-to-end)', { timeout: 180000 }, async () => {
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
