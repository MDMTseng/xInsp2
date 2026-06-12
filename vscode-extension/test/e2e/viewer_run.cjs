// viewer_run.cjs — drive the Run button on io_stress and confirm the Viewer
// shows the VARs (the fix: vars land even though the Viewer wasn't pre-opened).

const vscode = require('vscode');
const path   = require('path');
const assert = require('assert');
const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const slash = (s) => s.split('\\').join('/');
const REPO  = path.resolve(__dirname, '..', '..', '..');
const PROJECT = path.join(REPO, 'examples', 'io_stress');
const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'viewer_run');

async function run() {
    console.log('\n=== VIEWER RUN (button) E2E ===\n');
    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    assert.ok(await api.waitConnected(20000), 'backend must connect');
    clearOldShots(screenshotDir, 'viewer_run');

    // Open io_stress (compiles synthetic_features + inspect.cpp).
    assert.ok((await vscode.commands.executeCommand('xinsp2.openProject', slash(PROJECT)))?.ok !== false, 'open ok');
    const cr = await api.sendCmd('compile_and_load', { path: slash(path.join(PROJECT, 'inspect.cpp')) });
    assert.ok(cr.ok, 'script compiled: ' + (cr.error || ''));
    await api.sendCmd('list_instances');
    // Open the script so it's the active editor (mirrors a user about to click Run).
    await vscode.window.showTextDocument(await vscode.workspace.openTextDocument(path.join(PROJECT, 'inspect.cpp')));
    await sleep(800);

    // The Viewer has NOT been opened yet — this is the exact scenario that used
    // to drop the vars.
    console.log('  viewer resolved before run?', api.viewerResolved());

    // Press the Run button (same command the ▶ toolbar icon fires).
    await vscode.commands.executeCommand('xinsp2.run');
    await sleep(2500);   // vars arrive async + the viewer reveals/renders

    const n = api.viewerVarCount();
    console.log('  viewer var count after run:', n);
    assert.ok(n > 5, `Viewer received the run's VARs (got ${n})`);
    console.log('  ✓ Viewer shows the VARs after pressing Run');

    shot('viewer_after_run');
    console.log('\n=== VIEWER RUN E2E PASSED ===');
}

module.exports = { run };
