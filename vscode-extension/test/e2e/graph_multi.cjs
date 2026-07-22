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
// The committed, openable example — this e2e doubles as its verification.
const PROJECT = path.join(REPO, 'qa', 'graph_demo');
const FRAME   = slash(path.join(PROJECT, 'frames', 'frame_00.png'));

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'graph_multi');

async function run() {
    console.log('\n=== GRAPH MULTI (connected pipeline) ===\n');
    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    assert.ok(await api.waitConnected(20000), 'backend must connect');
    clearOldShots(screenshotDir, 'graph_multi');

    const opened = await vscode.commands.executeCommand('xinsp2.openProject', slash(PROJECT));
    assert.ok(opened && opened.ok !== false, 'open_project ok');
    const cr = await api.sendCmd('compile_and_load', { path: slash(path.join(PROJECT, 'inspect.cpp')) });
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
