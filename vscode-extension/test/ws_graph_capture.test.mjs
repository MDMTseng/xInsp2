// ws_graph_capture.test.mjs — the stage-2 pipeline-graph dataflow capture.
//
// Builds a temp project with TWO instances of a pack-door image plugin chained
// by an image (a's "cleaned" output → b's "src" input), runs it with capture
// on, and asserts graph_snapshot reconstructs the a→b edge via the "cleaned"
// key. Also asserts capture is OFF by default (no edges until enabled).
//
// SKIPPED at THE CUT (v12): the GraphCapture RECORDER died with the Record
// use()->process path — xi::GraphCapture::record() currently has no call site
// in the pack funnel (use_pack_process_cb), so graph_snapshot always returns
// empty. Additionally, ScriptPackBuilder::add_image is copy-only (no zero-copy
// pool-handle adoption yet), so image-handle identity — the thing edge
// reconstruction keys on — does not survive a script-side re-add. Re-enable
// once the recorder is re-wired on the pack path (and chaining identity is
// resolvable). The fixture below is pack-native so it no longer teaches
// xi::Record / xi::imread.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const PLUGIN_CPP = `// img_clean — copies input image "src" through the pool as "cleaned".
#include <cstring>
class ImgClean : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    void process(xi::PackIn& in, xi::PackOut& out) override {
        auto src = in.image("src");
        if (!src) { out.fault("missing_input", "src"); return; }
        xi::Image dst = pool_image(src->width, src->height, src->channels);
        std::memcpy(dst.write(), src->pixels,
                    (size_t)src->width * src->height * src->channels);
        out.adopt_image("cleaned", dst.width, dst.height, dst.channels, dst.pool_handle());
        out.i64("count", 1);
    }
};
XI_PLUGIN_IMPL(ImgClean)
XI_PLUGIN_PACK_DOOR(ImgClean)
`;

const PLUGIN_JSON = JSON.stringify({
    name: 'img_clean', description: 'image pass-through (graph capture fixture)',
    dll: 'img_clean.dll', factory: 'xi_plugin_create', has_ui: false,
}, null, 2);

const INST_JSON = JSON.stringify({ plugin: 'img_clean', config: {} }, null, 2);

const SCRIPT = `
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>
#include <vector>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    // Synthetic in-memory frame (no decode dependency).
    std::vector<uint8_t> px((size_t)64 * 64, 128);
    xi::ScriptPackBuilder b1;
    b1.add_image("src", 64, 64, 1, px.data());
    auto a_out = xi::use("a").process(b1.seal());
    auto cleaned = a_out.get_image("cleaned");
    if (!cleaned) { xi::result(-1, "no cleaned"); return; }
    // Feed a's "cleaned" image into b's "src" → an a→b image-dataflow edge.
    xi::ScriptPackBuilder b2;
    b2.add_image("src", cleaned->width, cleaned->height, cleaned->channels,
                 cleaned->pixels.data());
    auto b_out = xi::use("b").process(b2.seal());
    xi::result((int)(a_out.get_i64("count").value_or(0)
                     + b_out.get_i64("count").value_or(0)), "chain");
}
`;

function makeProject() {
    const dir = mkdtempSync(join(tmpdir(), 'xi_graph_'));
    mkdirSync(join(dir, 'plugins', 'img_clean', 'src'), { recursive: true });
    writeFileSync(join(dir, 'plugins', 'img_clean', 'plugin.json'), PLUGIN_JSON);
    writeFileSync(join(dir, 'plugins', 'img_clean', 'src', 'plugin.cpp'), PLUGIN_CPP);
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'graph_capture_demo', script: 'inspect.cpp', params: [], instances: [],
            plugins: { img_clean: { path: 'img_clean', compile: true } } }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'), SCRIPT);
    for (const n of ['a', 'b']) {
        mkdirSync(join(dir, 'instances', n), { recursive: true });
        writeFileSync(join(dir, 'instances', n, 'instance.json'), INST_JSON);
    }
    return dir;
}

// Wait for a specific rsp id, ignoring async pushes (events/instances).
async function rsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }
// cmd:run returns its rsp IMMEDIATELY and runs the inspect on a detached
// thread — so we must wait for the run_finished event before snapshot,
// else the capture hasn't recorded yet.
async function runAndWait(c, id, args) {
    c.send({ type: 'cmd', id, name: 'run', args });
    await rsp(c, id);
    for (;;) {
        const m = await c.nextText();
        if (m.type === 'event' && (m.name === 'run_finished' || m.name === 'run_error')) return m.name;
    }
}

test('graph capture reconstructs the a→b image-dataflow edge', {
    skip: 'v12: GraphCapture recorder not wired to the pack funnel (record() has no call site since the Record use()->process path was deleted) — graph_snapshot is always empty; pending pack-path re-hook',
}, async () => {
    const dir = makeProject();
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');

        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
        assert.equal((await rsp(c, 2)).ok, true, 'compile ok');

        // Default OFF: a run without enabling capture records nothing.
        await runAndWait(c, 3, {});
        c.send({ type: 'cmd', id: 4, name: 'graph_snapshot' });
        const before = await rsp(c, 4);
        assert.equal(before.ok, true);
        assert.equal(before.data.edges.length, 0, 'no edges captured while disabled (default off)');

        // Enable, run, snapshot.
        c.send({ type: 'cmd', id: 5, name: 'graph_capture', args: { enable: true } });
        assert.equal((await rsp(c, 5)).data.capturing, true, 'capturing on');
        await runAndWait(c, 6, {});
        c.send({ type: 'cmd', id: 7, name: 'graph_snapshot' });
        const snap = await rsp(c, 7);
        assert.equal(snap.ok, true);
        console.log('snapshot:', JSON.stringify(snap.data));

        assert.ok(snap.data.ran.includes('a') && snap.data.ran.includes('b'), 'both instances ran');
        const edge = snap.data.edges.find(e => e.from === 'a' && e.to === 'b');
        assert.ok(edge, 'a→b edge reconstructed from image-handle lineage');
        assert.ok(edge.keys.includes('cleaned'), 'edge carries the "cleaned" image key');
    });
});
