// graph_multi.cjs — a 3-node connected pipeline, rendered WITH edges, for a
// screenshot that actually shows the connection lines.
//
// Three blob_centroid_detector instances chained by image handles
// (a.cleaned → b.src → b.cleaned → c.src) → edges a→b and b→c. Opens the graph,
// captures the dataflow, repaints with edges, and screenshots it.

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const os     = require('os');
const assert = require('assert');

const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const slash = (s) => s.split('\\').join('/');
const REPO  = path.resolve(__dirname, '..', '..', '..');
const BLOB  = path.join(REPO, 'examples', 'blob_tracker');
const FRAME = slash(path.join(BLOB, 'frames', 'frame_00.png'));

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'graph_multi');

const INST = JSON.stringify({ plugin: 'blob_centroid_detector', config: { blur_radius: 2 } });
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  xi::Image frame = xi::imread(xi::current_frame_path());
  if (frame.empty()) return;
  auto a = xi::use("a");
  auto b = xi::use("b");
  auto c = xi::use("c");
  auto ao = a.process(xi::Record().image("src", frame));
  VAR(a_count, ao["count"].as_int(0));
  auto bo = b.process(xi::Record().image("src", ao.get_image("cleaned")));
  VAR(b_count, bo["count"].as_int(0));
  auto co = c.process(xi::Record().image("src", bo.get_image("cleaned")));
  VAR(c_count, co["count"].as_int(0));
}`;

function makeProject() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'xi_multi_'));
    fs.cpSync(path.join(BLOB, 'plugins', 'blob_centroid_detector'),
              path.join(dir, 'plugins', 'blob_centroid_detector'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'project.json'),
        JSON.stringify({ name: 'graph_multi', script: 'inspect.cpp', params: [], instances: [] }));
    fs.writeFileSync(path.join(dir, 'inspect.cpp'), SCRIPT);
    // A frames/ dir so the capture button's sample-frame auto-pick works.
    fs.mkdirSync(path.join(dir, 'frames'), { recursive: true });
    fs.copyFileSync(path.join(BLOB, 'frames', 'frame_00.png'), path.join(dir, 'frames', 'frame_00.png'));
    for (const n of ['a', 'b', 'c']) {
        fs.mkdirSync(path.join(dir, 'instances', n), { recursive: true });
        fs.writeFileSync(path.join(dir, 'instances', n, 'instance.json'), INST);
    }
    return dir;
}

async function run() {
    console.log('\n=== GRAPH MULTI (connected pipeline) ===\n');
    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    assert.ok(await api.waitConnected(20000), 'backend must connect');
    clearOldShots(screenshotDir, 'graph_multi');

    const dir = makeProject();
    const opened = await vscode.commands.executeCommand('xinsp2.openProject', slash(dir));
    assert.ok(opened && opened.ok !== false, 'open_project ok');
    const cr = await api.sendCmd('compile_and_load', { path: slash(path.join(dir, 'inspect.cpp')) });
    assert.ok(cr.ok, 'compile ok');
    await api.sendCmd('list_instances');
    await sleep(1500);

    // Open the graph, then capture + repaint with edges.
    await vscode.commands.executeCommand('xinsp2.openPipelineGraph');
    await sleep(800);
    const edges = await api.captureGraphEdges(FRAME);
    console.log('  edges:', JSON.stringify(edges.edges));
    const ab = edges.edges.find(e => e.from === 'a' && e.to === 'b');
    const bc = edges.edges.find(e => e.from === 'b' && e.to === 'c');
    assert.ok(ab && bc, 'a→b and b→c edges both reconstructed');
    console.log('  ✓ a→b→c chain captured');

    // Repaint the open panel with edges and screenshot the connection lines.
    await api.captureAndRenderGraph();
    await sleep(1200);
    shot('connected');
    console.log('\n=== GRAPH MULTI PASSED ===');
}

module.exports = { run };
