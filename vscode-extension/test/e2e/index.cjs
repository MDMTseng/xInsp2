// E2E test that runs INSIDE the VS Code Extension Host process.
// Called by @vscode/test-electron via require(). Must export run().

const vscode = require('vscode');
const path   = require('path');
const assert = require('assert');
const { execSync } = require('child_process');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

function takeScreenshot(filepath) {
    const fs = require('fs');
    const os = require('os');
    const psScript = path.join(os.tmpdir(), 'xinsp2_screenshot.ps1');
    fs.writeFileSync(psScript, `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$pt = New-Object System.Drawing.Point(0, 0)
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($bounds.Location, $pt, $bounds.Size)
$bmp.Save("${filepath.replace(/\\/g, '\\\\')}")
$gfx.Dispose()
$bmp.Dispose()
`);
    try {
        execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psScript}"`, { timeout: 15000 });
        console.log('[e2e] screenshot saved:', filepath);
    } catch (e) {
        console.log('[e2e] screenshot failed:', e.message);
    }
}

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

    // Focus the xInsp2 sidebar first so the webview loads its HTML
    try {
        await vscode.commands.executeCommand('xinsp2.viewer.focus');
    } catch {}
    await sleep(2000);

    // Run inspection — the webview is now ready to receive postMessage
    console.log('[e2e] running inspection...');
    await vscode.commands.executeCommand('xinsp2.run');
    await sleep(3000);
    console.log('[e2e] PASS: inspection executed');

    // Run a second time to be sure the viewer catches it
    await vscode.commands.executeCommand('xinsp2.run');
    await sleep(3000);

    // Take screenshot of the final state
    const screenshotDir = path.resolve(wf[0].uri.fsPath, '..', 'screenshot');
    require('fs').mkdirSync(screenshotDir, { recursive: true });
    const screenshotPath = path.join(screenshotDir, `e2e_${Date.now()}.png`);
    takeScreenshot(screenshotPath);

    console.log('[e2e] ALL E2E CHECKS PASSED');
}

module.exports = { run };
