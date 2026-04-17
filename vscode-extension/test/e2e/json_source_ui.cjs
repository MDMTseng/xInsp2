// One-off: open the json_source UI, populate sample fields, screenshot.
const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execSync } = require('child_process');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');

let stepNum = 0;
function shot(label) {
    stepNum++;
    fs.mkdirSync(screenshotDir, { recursive: true });
    const fp = path.join(screenshotDir, `json_source_${String(stepNum).padStart(2,'0')}_${label}.png`);
    const psScript = path.join(os.tmpdir(), 'xinsp2_json_ss.ps1');
    fs.writeFileSync(psScript, `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$pt = New-Object System.Drawing.Point(0, 0)
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($bounds.Location, $pt, $bounds.Size)
$bmp.Save("${fp.replace(/\\/g, '\\\\')}")
$gfx.Dispose()
$bmp.Dispose()
`);
    try {
        execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psScript}"`, { timeout: 15000 });
        console.log('  📸', path.basename(fp));
    } catch (e) {
        console.log('  📸 failed:', e.message);
    }
}

async function run() {
    console.log('\n=== JSON SOURCE UI SHOWCASE ===\n');

    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports?.__test__;

    await sleep(4000);
    if (api) await api.waitConnected(10000);

    // Clean prior shots
    try {
        for (const f of fs.readdirSync(screenshotDir)) {
            if (f.startsWith('json_source_')) fs.unlinkSync(path.join(screenshotDir, f));
        }
    } catch {}

    const projDir = path.join(os.tmpdir(), `xinsp2_json_${Date.now()}`);
    await vscode.commands.executeCommand('xinsp2.createProject', projDir, 'json_demo');
    await vscode.commands.executeCommand('xinsp2.createInstance', 'config0', 'json_source');
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1200);

    // Open the json_source webview UI
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'config0', 'json_source');
    await sleep(1500);
    shot('a_initial_empty');

    // The UI starts with one empty row. Configure it as a string field.
    api.setInputInWebview('config0', '#rows tr.field:nth-child(1) input[type=text]:first-of-type', 'name');
    await sleep(150);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(1) td.col-value input[type=text]', 'inspector_01');
    await sleep(400);
    shot('b_first_field_string');

    // Add a number field
    api.clickInWebview('config0', 'button.btn-add');
    await sleep(400);
    // Set the new row's key + select 'number' from the type dropdown via setInput
    api.setInputInWebview('config0', '#rows tr.field:nth-child(2) input[type=text]:first-of-type', 'threshold');
    await sleep(150);
    // The select dropdown — set value via setInput (works for <select>)
    api.setInputInWebview('config0', '#rows tr.field:nth-child(2) select', 'number');
    await sleep(400);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(2) td.col-value input[type=text]', '128');
    await sleep(400);
    shot('c_number_field_added');

    // Add a bool field
    api.clickInWebview('config0', 'button.btn-add');
    await sleep(400);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(3) input[type=text]:first-of-type', 'enabled');
    await sleep(150);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(3) select', 'bool');
    await sleep(300);
    api.clickInWebview('config0', '#rows tr.field:nth-child(3) input[type=checkbox]');
    await sleep(400);
    shot('d_bool_field_added');

    // Add a nested object field
    api.clickInWebview('config0', 'button.btn-add');
    await sleep(400);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(4) input[type=text]:first-of-type', 'roi');
    await sleep(150);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(4) select', 'object');
    await sleep(300);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(4) td.col-value input[type=text]',
        '{"x":10,"y":20,"w":640,"h":480}');
    await sleep(400);
    shot('e_object_field_added');

    // Add an array field
    api.clickInWebview('config0', 'button.btn-add');
    await sleep(400);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(5) input[type=text]:first-of-type', 'tags');
    await sleep(150);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(5) select', 'array');
    await sleep(300);
    api.setInputInWebview('config0', '#rows tr.field:nth-child(5) td.col-value input[type=text]',
        '["red","yellow","green"]');
    await sleep(500);
    shot('f_array_field_added');

    // Click Apply
    api.clickInWebview('config0', 'button.btn-apply');
    await sleep(800);
    shot('g_after_apply');

    // Verify the backend stored what we configured
    const status = await api.sendCmd('exchange_instance', {
        name: 'config0', cmd: { command: 'get_status' }
    });
    console.log('\nStored data after Apply:', JSON.stringify(status.data, null, 2));

    console.log('\n=== DONE — see screenshot/json_source_*.png ===');
}

module.exports = { run };
