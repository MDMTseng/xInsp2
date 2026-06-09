// ws_graph_capture.test.mjs — proves the stage-2 pipeline-graph dataflow capture.
//
// Builds a temp project with TWO instances of blob_centroid_detector chained by
// an image handle (a's "cleaned" output → b's "src" input), runs it with capture
// on, and asserts graph_snapshot reconstructs the a→b edge via the "cleaned"
// key. Also asserts capture is OFF by default (no edges until enabled).
//
// Edges are reconstructed by image-handle identity at the host use() callback —
// see service_main.cpp graph_record_call_ / "graph_snapshot".

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, cpSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { withBackend } from './helpers/client.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const slash     = (s) => s.split('\\').join('/');   // imread + JSON-safe paths
const EXAMPLES  = resolve(__dirname, '..', '..', 'examples');
const BLOB      = join(EXAMPLES, 'blob_tracker');
const FRAME     = slash(join(BLOB, 'frames', 'frame_00.png'));

const INST_JSON = JSON.stringify({
    plugin: 'blob_centroid_detector',
    config: { blur_radius: 2, block_radius: 40, diff_C: 15, close_radius: 1, min_area: 200, max_area: 4000 },
}, null, 2);

const SCRIPT = `
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto a = xi::use("a");
    auto b = xi::use("b");
    xi::Image frame = xi::imread(xi::current_frame_path());
    if (frame.empty()) { VAR(error, std::string("no frame")); return; }
    auto a_out = a.process(xi::Record().image("src", frame));
    // Feed a's "cleaned" image into b's "src" → an a→b image-dataflow edge.
    auto b_out = b.process(xi::Record().image("src", a_out.get_image("cleaned")));
    VAR(a_count, a_out["count"].as_int(0));
    VAR(b_count, b_out["count"].as_int(0));
}
`;

function makeProject() {
    const dir = mkdtempSync(join(tmpdir(), 'xi_graph_'));
    cpSync(join(BLOB, 'plugins', 'blob_centroid_detector'),
           join(dir, 'plugins', 'blob_centroid_detector'), { recursive: true });
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'graph_capture_demo', script: 'inspect.cpp', params: [], instances: [] }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'), SCRIPT);
    for (const n of ['a', 'b']) {
        mkdirSync(join(dir, 'instances', n), { recursive: true });
        writeFileSync(join(dir, 'instances', n, 'instance.json'), INST_JSON);
    }
    return dir;
}

// Wait for a specific rsp id, ignoring async pushes (vars/events/instances).
async function rsp(c, id) { for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === id) return m; } }
// cmd:run returns its rsp IMMEDIATELY (hardcoded ms:0) and runs the inspect on a
// detached thread — so we must wait for the run_finished event before snapshot,
// else the capture hasn't recorded yet.
async function runAndWait(c, id, args) {
    c.send({ type: 'cmd', id, name: 'run', args });
    await rsp(c, id);
    for (;;) {
        const m = await c.nextText();
        if (m.type === 'event' && (m.name === 'run_finished' || m.name === 'run_error')) return m.name;
    }
}

test('graph capture reconstructs the a→b image-dataflow edge', async () => {
    const dir = makeProject();
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
        assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');

        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
        assert.equal((await rsp(c, 2)).ok, true, 'compile ok');

        // Default OFF: a run without enabling capture records nothing.
        await runAndWait(c, 3, { frame_path: FRAME });
        c.send({ type: 'cmd', id: 4, name: 'graph_snapshot' });
        const before = await rsp(c, 4);
        assert.equal(before.ok, true);
        assert.equal(before.data.edges.length, 0, 'no edges captured while disabled (default off)');

        // Enable, run, snapshot.
        c.send({ type: 'cmd', id: 5, name: 'graph_capture', args: { enable: true } });
        assert.equal((await rsp(c, 5)).data.capturing, true, 'capturing on');
        await runAndWait(c, 6, { frame_path: FRAME });
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
