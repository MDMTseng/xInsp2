// webui_screenshot.cjs — E2E that opens a plugin's webui panel and proves
// it actually rendered, with a screenshot for human spot-check.
//
// What it proves (repeatably, without the flaky interactive window):
//   1. With a project open + backend connected, xinsp2.openInstanceUI opens
//      the instance's webview panel (a real editor tab appears).
//   2. The backend served the plugin's UI HTML (get_plugin_ui → ui_path).
//   3. A PrintWindow screenshot of the dev-host window is captured so a
//      human can eyeball the rendered panel.
//
// Why: during interactive testing the plugin webui "disappeared". This makes
// the open-webui path a CI-able check instead of a manual click.
//
// Fixture: examples/blob_tracker — instance "det" → blob_centroid_detector,
// which has has_ui:true and ships a ui/ folder. Read-only (never recompiles),
// so it coexists with the user's already-running :7823 backend.

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const assert = require('assert');

const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const REPO    = path.resolve(__dirname, '..', '..', '..');
const PROJECT = path.join(REPO, 'qa', 'blob_tracker');
const INSTANCE = 'det';
const PLUGIN   = 'blob_centroid_detector';

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'webui');

// Is there an open editor tab whose label matches the webui panel title
// (`det (blob_centroid_detector)`)? That tab only exists if the webview
// panel actually got created — a public-API proof the UI opened.
function webuiTabOpen() {
    const title = `${INSTANCE} (${PLUGIN})`;
    for (const group of vscode.window.tabGroups.all) {
        for (const tab of group.tabs) {
            if (tab.label === title) return true;
        }
    }
    return false;
}

async function run() {
    console.log('\n=== WEBUI SCREENSHOT E2E ===\n');

    const ext = vscode.extensions.all.find(e =>
        e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    if (!api) throw new Error('extension did not export __test__ API');

    console.log('Waiting for backend to connect...');
    assert.ok(await api.waitConnected(20000), 'backend must connect');
    console.log('  ✓ connected');

    clearOldShots(screenshotDir, 'webui');

    // Open the fixture so the persisted "det" instance + its plugin load.
    const opened = await api.sendCmd('open_project', { folder: PROJECT });
    assert.ok(opened && opened.ok !== false, 'open_project should succeed');
    await api.sendCmd('list_instances');
    await sleep(2000);

    // The plugin must actually advertise a UI, else there's nothing to open.
    const uiRsp = await api.sendCmd('get_plugin_ui', { plugin: PLUGIN });
    assert.ok(uiRsp.ok && uiRsp.data && uiRsp.data.ui_path,
        'get_plugin_ui must return a ui_path: ' + JSON.stringify(uiRsp));
    assert.ok(fs.existsSync(path.join(uiRsp.data.ui_path, 'index.html')),
        'plugin ui/index.html must exist on disk');
    console.log('  ✓ backend serves UI at', uiRsp.data.ui_path);

    shot('before_open');

    // Open the webui the same way a tree button / Ctrl-click does.
    console.log(`Opening webui for ${INSTANCE} (${PLUGIN})...`);
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', INSTANCE, PLUGIN);
    // The panel HTML + in-house xi-* kit bundle need a beat to render.
    await sleep(3000);

    assert.ok(webuiTabOpen(),
        `webui panel tab "${INSTANCE} (${PLUGIN})" must be open after openInstanceUI`);
    console.log('  ✓ webui panel tab is open');

    shot('webui_open');

    // The screenshot is the deliverable — make sure it landed and isn't a
    // zero-byte stub.
    const shots = fs.readdirSync(screenshotDir)
        .filter(f => f.startsWith('webui_') && f.includes('webui_open') && f.endsWith('.png'));
    assert.ok(shots.length > 0, 'a webui_open screenshot must be written');
    const sz = fs.statSync(path.join(screenshotDir, shots[0])).size;
    console.log(`  ✓ screenshot ${shots[0]} (${sz} bytes)`);
    assert.ok(sz > 5000, 'screenshot should be a non-trivial PNG');

    console.log('\n=== WEBUI SCREENSHOT E2E PASSED ===');
    console.log('screenshots in:', screenshotDir);
}

module.exports = { run };
