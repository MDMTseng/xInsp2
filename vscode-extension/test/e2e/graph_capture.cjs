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

// NOTE (v12 / THE CUT): the GraphCapture RECORDER is currently unwired — it
// recorded on the deleted Record use()->process path and has no call site in
// the pack funnel yet, so captureGraphEdges() returns empty and this journey's
// edge assertions FAIL until the recorder is re-hooked on the pack path. The
// fixture below is pack-native (embedded pack-door plugin + ScriptPackBuilder
// script) so it no longer teaches xi::Record / xi::imread.

const slash   = (s) => s.split('\\').join('/');
const REPO    = path.resolve(__dirname, '..', '..', '..');
const FRAME   = '';   // no decode dependency — the script builds its frame in memory

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'graph_capture');

const PLUGIN_JSON = JSON.stringify({
    name: 'img_clean', description: 'image pass-through (graph capture fixture)',
    dll: 'img_clean.dll', factory: 'xi_plugin_create', has_ui: false,
}, null, 2);
const PLUGIN_CPP = `#include <cstring>
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

const INST = JSON.stringify({ plugin: 'img_clean', config: {} });
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <vector>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int){
  std::vector<uint8_t> px((size_t)64 * 64, 128);
  xi::ScriptPackBuilder b1;
  b1.add_image("src", 64, 64, 1, px.data());
  auto a_out = xi::use("a").process(b1.seal());
  auto cleaned = a_out.get_image("cleaned");
  if (!cleaned) return;
  xi::ScriptPackBuilder b2;
  b2.add_image("src", cleaned->width, cleaned->height, cleaned->channels,
               cleaned->pixels.data());
  xi::use("b").process(b2.seal());
}`;

function makeProject() {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'xi_egraph_'));
    fs.mkdirSync(path.join(dir, 'plugins', 'img_clean', 'src'), { recursive: true });
    fs.writeFileSync(path.join(dir, 'plugins', 'img_clean', 'plugin.json'), PLUGIN_JSON);
    fs.writeFileSync(path.join(dir, 'plugins', 'img_clean', 'src', 'plugin.cpp'), PLUGIN_CPP);
    fs.writeFileSync(path.join(dir, 'project.json'),
        JSON.stringify({ name: 'egraph', script: 'inspect.cpp', params: [], instances: [],
            plugins: { img_clean: { path: 'img_clean', compile: true } } }));
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
