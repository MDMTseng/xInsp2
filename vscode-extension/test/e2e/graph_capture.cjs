// graph_capture.cjs — E2E for the stage-2 pipeline-graph dataflow edges.
//
// Builds a temp project with two chained blob_centroid_detector instances
// (a's "cleaned" → b's "src"), opens it, then through the extension's test API:
//   1. captureGraphEdges() runs once with capture on and reconstructs the a→b
//      image-dataflow edge (via "cleaned").
//   2. renderPipelineGraphHtml(nodes, edges) emits the SVG connector + key label.
//   3. The graph webview opens + is screenshotted.

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const os     = require('os');
const assert = require('assert');

const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const slash   = (s) => s.split('\\').join('/');
const REPO    = path.resolve(__dirname, '..', '..', '..');
const BLOB    = path.join(REPO, 'examples', 'blob_tracker');
const FRAME   = slash(path.join(BLOB, 'frames', 'frame_00.png'));

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'graph_capture');

const INST = JSON.stringify({ plugin: 'blob_centroid_detector', config: { blur_radius: 2 } });
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  xi::Image frame = xi::imread(xi::current_frame_path());
  if (frame.empty()) return;
  auto a = xi::use("a"); auto b = xi::use("b");
  auto a_out = a.process(xi::Record().image("src", frame));
  auto b_out = b.process(xi::Record().image("src", a_out.get_image("cleaned")));
}`;

function makeProject() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'xi_egraph_'));
    fs.cpSync(path.join(BLOB, 'plugins', 'blob_centroid_detector'),
              path.join(dir, 'plugins', 'blob_centroid_detector'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'project.json'),
        JSON.stringify({ name: 'egraph', script: 'inspect.cpp', params: [], instances: [] }));
    fs.writeFileSync(path.join(dir, 'inspect.cpp'), SCRIPT);
    for (const n of ['a', 'b']) {
        fs.mkdirSync(path.join(dir, 'instances', n), { recursive: true });
        fs.writeFileSync(path.join(dir, 'instances', n, 'instance.json'), INST);
    }
    return dir;
}

async function run() {
    console.log('\n=== GRAPH CAPTURE (stage 2) E2E ===\n');
    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    if (!api) throw new Error('no __test__ API');
    assert.ok(await api.waitConnected(20000), 'backend must connect');
    console.log('  ✓ connected');

    clearOldShots(screenshotDir, 'graph_capture');

    const dir = makeProject();
    const opened = await vscode.commands.executeCommand('xinsp2.openProject', slash(dir));
    assert.ok(opened && opened.ok !== false, 'open_project ok');
    const cr = await api.sendCmd('compile_and_load', { path: slash(path.join(dir, 'inspect.cpp')) });
    assert.ok(cr.ok, 'compile ok');
    await api.sendCmd('list_instances');
    await sleep(1500);

    // 1) Capture the dataflow edge through the extension API.
    const { ran, edges } = await api.captureGraphEdges(FRAME);
    console.log('  captured:', JSON.stringify({ ran, edges }));
    assert.ok(ran.includes('a') && ran.includes('b'), 'both instances ran');
    const edge = edges.find(e => e.from === 'a' && e.to === 'b');
    assert.ok(edge, 'a→b dataflow edge captured');
    assert.ok((edge.keys || []).includes('cleaned'), 'edge carries the "cleaned" image key');
    console.log('  ✓ a→b edge via "cleaned"');

    // 2) Render with edges → SVG connector + label present.
    const nodes = api.extractPipelineNodes();
    const html = api.renderPipelineGraphHtml(nodes, edges);
    assert.ok(html.includes('class="eline"') || html.includes("class='eline'") || html.includes('EDGES = [{'),
        'rendered HTML carries the edge data for SVG drawing');
    assert.ok(html.includes('cleaned'), 'rendered HTML includes the edge key label');
    console.log('  ✓ renderPipelineGraphHtml emits the edge');

    // 3) Open the panel + screenshot.
    await vscode.commands.executeCommand('xinsp2.openPipelineGraph');
    await sleep(1500);
    shot('graph_nodes');
    console.log('\n=== GRAPH CAPTURE (stage 2) E2E PASSED ===');
}

module.exports = { run };
