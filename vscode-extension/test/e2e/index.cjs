// E2E test that runs INSIDE the VS Code Extension Host process.
// Called by @vscode/test-electron via require(). Must export run().

const vscode = require('vscode');
const path   = require('path');
const assert = require('assert');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

async function run() {
    console.log('[e2e] starting xInsp2 automated test...');

    // Wait for extension to activate and backend to connect
    let ext = vscode.extensions.all.find(e =>
        e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found among: ' +
        vscode.extensions.all.map(e => e.id).filter(id => !id.startsWith('vscode.')).join(', '));

    if (!ext.isActive) {
        console.log('[e2e] activating extension...');
        await ext.activate();
    }
    console.log('[e2e] extension active:', ext.id);

    // Give backend time to start + connect
    console.log('[e2e] waiting for backend...');
    await sleep(4000);

    // Verify commands registered
    const cmds = await vscode.commands.getCommands(true);
    assert.ok(cmds.includes('xinsp2.run'),     'xinsp2.run registered');
    assert.ok(cmds.includes('xinsp2.compile'),  'xinsp2.compile registered');
    assert.ok(cmds.includes('xinsp2.saveProject'), 'xinsp2.saveProject registered');
    assert.ok(cmds.includes('xinsp2.loadProject'), 'xinsp2.loadProject registered');
    console.log('[e2e] PASS: 4 commands registered');

    // Open the example script in the editor
    const wf = vscode.workspace.workspaceFolders;
    if (!wf || !wf.length) throw new Error('no workspace folder');
    const scriptPath = path.join(wf[0].uri.fsPath, 'user_script_example.cpp');
    console.log('[e2e] opening', scriptPath);
    const doc = await vscode.workspace.openTextDocument(scriptPath);
    await vscode.window.showTextDocument(doc);
    console.log('[e2e] PASS: script opened');

    // Compile
    console.log('[e2e] compiling (this takes ~5-30s)...');
    await vscode.commands.executeCommand('xinsp2.compile');
    // Compile is async behind a WS round-trip; wait for it
    await sleep(30000);
    console.log('[e2e] compile wait done');

    // Run inspection
    console.log('[e2e] running inspection...');
    await vscode.commands.executeCommand('xinsp2.run');
    await sleep(3000);
    console.log('[e2e] PASS: inspection executed');

    // Focus the xInsp2 sidebar to make it visible
    try {
        await vscode.commands.executeCommand('xinsp2.instances.focus');
    } catch {}
    await sleep(1000);

    // Leave VS Code open for visual inspection (or screenshot)
    console.log('[e2e] keeping window open for 5s...');
    await sleep(5000);

    console.log('[e2e] ALL E2E CHECKS PASSED');
}

module.exports = { run };
