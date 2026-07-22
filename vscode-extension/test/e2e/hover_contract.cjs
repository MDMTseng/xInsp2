// hover_contract.cjs — E2E for the use("…") plugin I/O contract hover.
//
// What it proves (repeatably, without the flaky interactive window):
//   1. With a project open + backend connected, hovering the instance name
//      in `xi::use("det")` returns the plugin's I/O contract (Inputs /
//      Outputs / Params), NOT the "not a known instance yet" fallback.
//   2. The contract is rendered as copyable code blocks (MarkdownString
//      with fenced ``` regions) and carries the real manifest fields
//      (e.g. blur_radius, centroids).
//
// Why a dedicated suite: the hover depends on instanceMap (from
// list_instances) + pluginManifests (from list_plugins). The managed
// backend the harness spawns connects reliably on its own free port, so
// this exercises the SAME population path the real editor uses — it just
// removes the human + the respawn-limit flakiness from the loop.
//
// Fixture: examples/blob_tracker. It has a persisted instance
// (instances/det/instance.json → blob_centroid_detector) and that plugin
// ships a full `manifest` block in plugin.json, plus inspect.cpp uses
// xi::use("det"). We only READ — never recompile — so loading the already
// built blob_centroid_detector.dll alongside the user's :7823 backend is a
// shared read, not a build lock.

const vscode = require('vscode');
const path   = require('path');
const assert = require('assert');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// repo/examples/blob_tracker — this file sits at vscode-extension/test/e2e/.
const REPO       = path.resolve(__dirname, '..', '..', '..');
const PROJECT    = path.join(REPO, 'qa', 'blob_tracker');
const SCRIPT     = path.join(PROJECT, 'inspect.cpp');

// Find the position of the instance name inside the first xi::use("name")
// in `text`. Returns { line, character, name } pointing INTO the name.
function findUsePosition(text) {
    const re = /\buse\s*(?:<[^>]*>)?\s*\(\s*"([^"]+)"\s*\)/;
    const m = re.exec(text);
    if (!m) throw new Error('no xi::use("…") found in script');
    const nameIdx = m.index + m[0].indexOf('"' + m[1] + '"') + 1; // first char of the name
    const before  = text.slice(0, nameIdx);
    const line     = before.split('\n').length - 1;
    const character = nameIdx - (before.lastIndexOf('\n') + 1);
    return { line, character, name: m[1] };
}

// Pull every plain-text segment out of an array of Hover results so we can
// assert on content regardless of MarkdownString vs MarkedString shape.
function hoverText(hovers) {
    const parts = [];
    for (const h of hovers || []) {
        for (const c of h.contents || []) {
            if (typeof c === 'string') parts.push(c);
            else if (c && typeof c.value === 'string') parts.push(c.value);
        }
    }
    return parts.join('\n---\n');
}

async function run() {
    console.log('\n=== HOVER CONTRACT E2E ===\n');

    const ext = vscode.extensions.all.find(e =>
        e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found among: ' +
        vscode.extensions.all.map(e => e.id).filter(id => !id.startsWith('vscode.')).join(', '));
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    if (!api) throw new Error('extension did not export __test__ API');

    console.log('Waiting for backend to connect...');
    const connected = await api.waitConnected(20000);
    assert.ok(connected, 'backend must connect (managed spawn on free port)');
    console.log('  ✓ connected');

    // Open the fixture project THROUGH the extension command (not a raw
    // sendCmd), so the same state a real UI open sets up — currentScriptPath,
    // instanceMap, manifest cache — is populated.
    console.log('Opening project:', PROJECT);
    const opened = await vscode.commands.executeCommand('xinsp2.openProject', PROJECT);
    assert.ok(opened && opened.ok !== false, 'open_project should succeed: ' + JSON.stringify(opened));

    // list_instances triggers an `instances` push, which is what populates
    // instanceMap AND refetches list_plugins → cachePluginManifests. Give
    // that round trip + the fire-and-forget refetch time to land.
    await api.sendCmd('list_instances');
    await sleep(2500);

    // Sanity: the backend itself reports det → blob_centroid_detector with a
    // manifest. If this fails, the fixture/scan is the problem, not the hover.
    const plugins = await api.sendCmd('list_plugins');
    const det = (plugins.data || []).find(p => p.name === 'blob_centroid_detector');
    assert.ok(det, 'blob_centroid_detector should be discovered');
    assert.ok(det.manifest && det.manifest.params, 'plugin should carry a manifest');
    console.log('  ✓ backend reports manifest (params/inputs/outputs present)');

    // The open project's script path must follow project.json (inspect.cpp),
    // NOT the hardcoded default — this is what the editor-title Compile/Run
    // gating (xinsp2.isActiveScript) keys off, so it must be name-correct.
    assert.ok(api.scriptPath && /inspect\.cpp$/i.test(api.scriptPath),
        `scriptPath should resolve to the project's inspect.cpp, got ${api.scriptPath}`);
    console.log('  ✓ script path follows project.json:', api.scriptPath);

    // Open the script and invoke the hover provider on the "det" in use("det").
    const doc = await vscode.workspace.openTextDocument(SCRIPT);
    await vscode.window.showTextDocument(doc);
    await sleep(300); // let onDidChangeActiveTextEditor settle
    assert.ok(api.activeIsScript,
        'with inspect.cpp active, xinsp2.isActiveScript must be true (editor Compile/Run show)');
    console.log('  ✓ active editor recognized as the script → toolbar buttons gate true');

    // Script-side file gating (drives both the toolbar gate and the
    // save→recompile-the-script trigger for multi-file scripts). Association is
    // by the script's stem prefix — inspect.cpp → inspect_*:
    //   - the script itself                 → true
    //   - a sibling inspect_*.hpp lane      → true (compiles into the script TU)
    //   - a NON-prefixed sibling header     → false (unrelated; no recompile)
    //   - a project PLUGIN source           → false (own hot-reload path)
    assert.ok(api.isProjectScriptFile(SCRIPT), 'script counts as a script file');
    assert.ok(api.isProjectScriptFile(path.join(PROJECT, 'inspect_lane_a.hpp')),
        'a sibling inspect_*.hpp lane counts as a script file (recompiles the script on save)');
    assert.ok(!api.isProjectScriptFile(path.join(PROJECT, 'helper.hpp')),
        'a non-prefixed sibling header is NOT associated (no spurious recompile)');
    assert.ok(!api.isProjectScriptFile(
        path.join(PROJECT, 'plugins', 'blob_centroid_detector', 'src', 'plugin.cpp')),
        'a project plugin source is NOT a script file (it has its own recompile path)');
    console.log('  ✓ script-side file gating: script + inspect_* lane true; non-prefixed + plugin source false');
    const pos = findUsePosition(doc.getText());
    console.log(`  use("${pos.name}") at ${pos.line}:${pos.character}`);
    assert.strictEqual(pos.name, 'det', 'fixture should hover over the det instance');

    const position = new vscode.Position(pos.line, pos.character);
    const hovers = await vscode.commands.executeCommand(
        'vscode.executeHoverProvider', doc.uri, position);

    const text = hoverText(hovers);
    console.log('\n----- hover markdown -----\n' + text + '\n--------------------------\n');

    // The whole point: NOT the fallback.
    assert.ok(!/not a known instance/i.test(text),
        'hover must resolve the instance, not show the unknown-instance fallback');

    // The contract sections are present...
    for (const section of ['Inputs', 'Outputs', 'Params']) {
        assert.ok(text.includes(section), `hover must include "${section}" section`);
    }
    // ...with real manifest fields...
    for (const field of ['src', 'centroids', 'blur_radius', 'min_area']) {
        assert.ok(text.includes(field), `hover must include manifest field "${field}"`);
    }
    // ...rendered as copyable fenced code blocks (the user's hard requirement:
    // they must be able to COPY, not just read).
    assert.ok((text.match(/```/g) || []).length >= 2,
        'contract must be in copyable fenced code blocks');

    console.log('=== HOVER CONTRACT E2E PASSED ===');
}

module.exports = { run };
