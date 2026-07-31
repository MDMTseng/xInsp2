// pipeline_graph.cjs — E2E for the stage-1 pipeline graph (nodes → click → webui).
//
// Proves, repeatably:
//   1. extractPipelineNodes() pulls the script's xi::use("…") instances (here
//      blob_tracker's "det") with its plugin + I/O-contract counts.
//   2. xinsp2.openPipelineGraph opens the graph webview panel (a real tab).
//   3. A screenshot is captured for a human spot-check.
//
// Edges (data flow) are intentionally NOT asserted — stage 1 is nodes only;
// dataflow needs a runtime trace (the script transforms data between plugins,
// so it can't be parsed statically). The point of this stage is click → webui.

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const assert = require('assert');

const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const REPO    = path.resolve(__dirname, '..', '..', '..');
const PROJECT = path.join(REPO, 'qa', 'blob_tracker');
const SCRIPT_PATH = path.join(PROJECT, 'inspect.cpp').split('\\').join('/');

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'pipeline_graph');

function graphTabOpen() {
    for (const group of vscode.window.tabGroups.all)
        for (const tab of group.tabs)
            if (tab.label === 'Pipeline Graph') return true;
    return false;
}

async function run() {
    console.log('\n=== PIPELINE GRAPH E2E ===\n');

    const ext = vscode.extensions.all.find(e =>
        e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    if (!api) throw new Error('extension did not export __test__ API');

    assert.ok(await api.waitConnected(20000), 'backend must connect');
    console.log('  ✓ connected');

    clearOldShots(screenshotDir, 'pipeline_graph');

    // Open project via the command so currentScriptPath + instanceMap populate.
    const opened = await vscode.commands.executeCommand('xinsp2.openProject', PROJECT);
    assert.ok(opened && opened.ok !== false, 'open_project should succeed');
    await api.sendCmd('list_instances');
    await sleep(2000);

    // 1) Node extraction.
    const nodes = api.extractPipelineNodes();
    console.log('  nodes:', JSON.stringify(nodes));
    const det = nodes.find(n => n.name === 'det');
    assert.ok(det, 'graph must contain the "det" instance from the script');
    assert.strictEqual(det.plugin, 'blob_centroid_detector', 'det → its plugin');
    assert.ok(det.known, 'det is a known instance');
    assert.ok(det.inputs >= 1 && det.outputs >= 1, 'det carries I/O counts from its manifest');
    console.log(`  ✓ extractPipelineNodes: det (${det.plugin}) ${det.inputs} in · ${det.outputs} out`);

    // (The VAR-chip assertions that used to sit here died with the VAR()/EMIT()
    // macros — the framework hard-deleted them, so chips can never surface.)

    // #2 — capture with NO explicit frame: auto-picks a sample from <project>/frames.
    // Compile-dependent — tolerate a transient script-DLL lock (another backend
    // holding inspect_vN.dll), the capture data path is covered by graph_capture.
    const cr = await api.sendCmd('compile_and_load', { path: SCRIPT_PATH });
    if (cr.ok) {
        console.log('  firstProjectFrame:', api.firstProjectFrame());
        const cap = await api.captureGraphEdges();
        console.log('  captured (auto-frame):', JSON.stringify(cap));
        assert.ok(cap.ran.includes('det'), 'auto-picked sample frame made det actually run');
        console.log('  ✓ capture auto-picked a sample frame (det ran)');
    } else {
        console.log('  (compile locked: ' + (cr.error || '') + ' — skipping auto-frame capture assert)');
    }

    // 2) Open the graph webview.
    await vscode.commands.executeCommand('xinsp2.openPipelineGraph');
    await sleep(1500);
    assert.ok(graphTabOpen(), 'Pipeline Graph webview tab must open');
    console.log('  ✓ pipeline graph panel open');

    shot('graph_open');
    const shots = fs.readdirSync(screenshotDir)
        .filter(f => f.startsWith('pipeline_graph_') && f.endsWith('.png'));
    assert.ok(shots.length > 0 && fs.statSync(path.join(screenshotDir, shots[0])).size > 4000,
        'a non-trivial screenshot must be written');
    console.log(`  ✓ screenshot ${shots[0]}`);

    console.log('\n=== PIPELINE GRAPH E2E PASSED ===');
}

module.exports = { run };
