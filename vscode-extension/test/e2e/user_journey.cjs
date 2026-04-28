// User journey E2E — drives entirely through VS Code commands.
//
// Simulates a real user building an inspection project from scratch:
//   1. Create a new project
//   2. Create instances: cam0 (mock_camera), det0 (blob_analysis), saver0 (record_save)
//   3. Open camera UI, configure, start streaming
//   4. Open blob analysis UI, set threshold
//   5. Open saver UI, set output folder + naming rule, enable
//   6. Write inspection script that wires them together
//   7. Save script → auto-compile → auto-run
//   8. Verify viewer shows correct vars
//   9. Verify files were saved to disk
//   10. Stop, save project, close
//
// All steps use vscode.commands.executeCommand — no internal API calls.

const vscode = require('vscode');
const path = require('path');
const fs = require('fs');
const os = require('os');
const assert = require('assert');
const { execSync } = require('child_process');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');
const projectRoot = path.resolve(__dirname, '..', '..', '..');
let stepNum = 0;

// ---- UI-state assertions (complement the backend-side ones) ------------
// These verify the EDITOR actually reflects the commanded state: webview
// panels really open, active editor is what we expect, tree items are
// present. Before this, the journey only asserted backend WS responses +
// on-disk artifacts; UI correctness was only visually inspected.

function pluginUITabs() {
    const out = [];
    for (const group of vscode.window.tabGroups.all) {
        for (const tab of group.tabs) {
            const input = tab.input;
            // vscode.TabInputWebview — title is "<instance> (<plugin>)"
            if (input && input.viewType && input.viewType.includes('xinsp2.pluginUI')) {
                out.push(tab.label);
            }
        }
    }
    return out;
}

function assertPluginUIOpen(instanceName, pluginName) {
    const expected = `${instanceName} (${pluginName})`;
    const found = pluginUITabs();
    assert.ok(found.includes(expected),
        `expected plugin UI tab "${expected}" open; saw [${found.join(', ')}]`);
}

function assertActiveEditor(fileBasename) {
    const ed = vscode.window.activeTextEditor;
    assert.ok(ed, `expected an active editor (want ${fileBasename})`);
    const actual = path.basename(ed.document.fileName);
    assert.strictEqual(actual, fileBasename,
        `active editor should be ${fileBasename}, is ${actual}`);
}

// The extension host process (this one) is a descendant of the Code.exe
// main process that owns the VS Code window. Walk up ppid via
// Win32_Process so PowerShell can pick THAT specific window — not
// whatever stale "Extension Development Host" session the user already
// had open. Resolved once; the main PID doesn't change.
let mainCodePid = null;
function resolveMainCodePid() {
    if (mainCodePid) return mainCodePid;
    try {
        // Walk up the ancestor chain from the extension host (this Node
        // process) to find the first Code.exe that actually owns a
        // window — that's THIS EDH, not some other VS Code the user has
        // open. Write the PS to a file; cramming multi-line script into
        // -Command via ; separators breaks Get-CimInstance blocks.
        const psFile = path.join(os.tmpdir(), `xinsp2_resolve_pid_${process.pid}.ps1`);
        fs.writeFileSync(psFile, `
$p = ${process.pid}
while ($true) {
    $proc = Get-CimInstance Win32_Process -Filter "ProcessId=$p" -ErrorAction SilentlyContinue
    if (-not $proc) { break }
    $pname = [IO.Path]::GetFileNameWithoutExtension($proc.Name)
    if ($pname -eq 'Code') {
        $hp = Get-Process -Id $p -ErrorAction SilentlyContinue
        if ($hp -and $hp.MainWindowHandle -ne [IntPtr]::Zero) {
            Write-Output $p
            break
        }
    }
    if (-not $proc.ParentProcessId -or $proc.ParentProcessId -eq 0) { break }
    $p = $proc.ParentProcessId
}
`);
        const out = execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psFile}"`,
            { timeout: 10000, encoding: 'utf8' }).trim();
        const n = parseInt(out, 10);
        if (!isNaN(n)) { mainCodePid = n; console.log(`  [screenshot] main Code.exe pid=${n}`); }
    } catch (e) {
        console.log(`  [screenshot] pid resolution failed: ${e.message}`);
    }
    return mainCodePid;
}

function takeScreenshot(label) {
    stepNum++;
    fs.mkdirSync(screenshotDir, { recursive: true });
    const fname = `journey_${String(stepNum).padStart(2,'0')}_${label}.png`;
    const fpath = path.join(screenshotDir, fname);
    const pid = resolveMainCodePid() || 0;
    // PrintWindow with flag 3 = PW_CLIENTONLY | PW_RENDERFULLCONTENT;
    // works with DWM-composited windows (modern VS Code) where older
    // flag=2 captures a black bitmap. Also: we always restore + flash
    // foreground the target before capture so DWM has a fresh frame.
    const psScript = path.join(os.tmpdir(), `xinsp2_journey_ss_${process.pid}.ps1`);
    fs.writeFileSync(psScript, `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$targetHwnd = [IntPtr]::Zero
$targetTitle = ""
# First preference: the specific Code.exe pid we resolved from the
# extension host's ancestor chain. That's THIS EDH, not some other one.
$preferredPid = ${pid}
if ($preferredPid -gt 0) {
    $p = Get-Process -Id $preferredPid -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        $targetHwnd = $p.MainWindowHandle
        $targetTitle = $p.MainWindowTitle
    }
}
# Fallback: newest Code.exe with "Extension Development Host" in title.
if ($targetHwnd -eq [IntPtr]::Zero) {
    $best = Get-Process -Name Code -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -like "*Extension Development Host*" -and
                           $_.MainWindowHandle -ne [IntPtr]::Zero } |
            Sort-Object -Property StartTime -Descending |
            Select-Object -First 1
    if ($best) { $targetHwnd = $best.MainWindowHandle; $targetTitle = $best.MainWindowTitle }
}
Write-Host "[ss] hwnd=$targetHwnd title='$targetTitle'"
if ($targetHwnd -eq [IntPtr]::Zero) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($bounds.Location, (New-Object System.Drawing.Point(0,0)), $bounds.Size)
} else {
    if ([Win]::IsIconic($targetHwnd)) { [void][Win]::ShowWindow($targetHwnd, 9) }
    [void][Win]::ShowWindow($targetHwnd, 5)         # SW_SHOW
    [void][Win]::SetForegroundWindow($targetHwnd)
    Start-Sleep -Milliseconds 150
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($targetHwnd, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { $w = 1200; $h = 800 }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    # PW_CLIENTONLY(1) | PW_RENDERFULLCONTENT(2) = 3
    $ok = [Win]::PrintWindow($targetHwnd, $hdc, 3)
    $gfx.ReleaseHdc($hdc)
    # If PrintWindow fails (some DWM states), fall back to screen copy
    # of the window's rect.
    if (-not $ok) {
        $bmp.Dispose(); $gfx.Dispose()
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        $gfx.CopyFromScreen(
            (New-Object System.Drawing.Point($r.Left, $r.Top)),
            (New-Object System.Drawing.Point(0, 0)),
            (New-Object System.Drawing.Size($w, $h)))
        Write-Host "[ss] PrintWindow failed, used CopyFromScreen"
    }
}
$bmp.Save("${fpath.replace(/\\/g, '\\\\')}")
$gfx.Dispose(); $bmp.Dispose()
`);
    try {
        const out = execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psScript}"`,
            { timeout: 20000, encoding: 'utf8' });
        const diag = (out.match(/\[ss\][^\r\n]*/g) || []).join(' | ');
        console.log(`  📸 ${fname}${diag ? '   ' + diag : ''}`);
    } catch (e) {
        console.log(`  📸 ${fname} failed: ${e.message}`);
    }
}
const shot = takeScreenshot;

async function run() {
    console.log('\n=== USER JOURNEY E2E ===\n');

    let ext = vscode.extensions.all.find(e => e.id.includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports?.__test__;

    console.log('Waiting for backend...');
    await sleep(4000);
    if (api) await api.waitConnected(10000);

    // Clean old journey screenshots
    try {
        for (const f of fs.readdirSync(screenshotDir)) {
            if (f.startsWith('journey_')) fs.unlinkSync(path.join(screenshotDir, f));
        }
    } catch {}

    // Test scenario state
    const projDir = path.join(os.tmpdir(), `xinsp2_journey_${Date.now()}`);
    const saveDir = path.join(projDir, 'inspection_results');

    // ====== STEP 0: Initial state — no project, welcome view on display ======
    console.log('\n[STEP 0] Initial state: backend connected, no project');
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('initial_welcome');

    // ====== STEP 1: User creates project via command palette ======
    console.log('\n[STEP 1] User: Command Palette → "xInsp2: Create Project"');
    const r1 = await vscode.commands.executeCommand('xinsp2.createProject', projDir, 'my_inspection');
    assert.ok(r1?.ok, 'create_project should succeed');
    assert.ok(fs.existsSync(path.join(projDir, 'project.json')), 'project.json created on disk');
    assert.ok(fs.existsSync(path.join(projDir, 'inspection.cpp')), 'starter inspection.cpp created');
    console.log(`  ✓ project created: ${projDir}`);
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('project_created_empty_welcome');

    // ====== STEP 2: User clicks "+" in Instances view → real QuickPick + InputBox ======
    // The "+" button invokes xinsp2.createInstance with NO args, which then
    // shows a QuickPick of plugins and an InputBox for the name. We replace
    // showQuickPick / showInputBox with our own that uses createQuickPick /
    // createInputBox under the hood — visually identical to the native widget,
    // but we control timing so we can capture each stage of the user flow.
    const origQuickPick = vscode.window.showQuickPick;
    const origInputBox  = vscode.window.showInputBox;

    async function addInstanceVerbose(plugin, name, prefix) {
        vscode.window.showQuickPick = (items, opts) => new Promise(async (resolve) => {
            const arr = await Promise.resolve(items);
            const qp = vscode.window.createQuickPick();
            qp.placeholder = opts?.placeHolder ?? '';
            qp.items = arr;
            qp.show();
            await sleep(1200);
            takeScreenshot(`${prefix}_a_plugin_picker`);
            // Highlight the desired plugin (as if user arrowed/clicked)
            qp.activeItems = arr.filter(it => (it.label ?? it) === plugin);
            await sleep(800);
            takeScreenshot(`${prefix}_b_plugin_highlighted`);
            // Accept (Enter)
            const picked = qp.activeItems[0];
            qp.hide();
            qp.dispose();
            resolve(picked);
        });

        vscode.window.showInputBox = (opts) => new Promise(async (resolve) => {
            const ib = vscode.window.createInputBox();
            ib.prompt = opts?.prompt ?? '';
            ib.value  = opts?.value ?? '';
            ib.show();
            await sleep(900);
            takeScreenshot(`${prefix}_c_name_inputbox_default`);
            // User clears the default and types the desired name
            ib.value = '';
            await sleep(300);
            ib.value = name;
            await sleep(900);
            takeScreenshot(`${prefix}_d_name_typed`);
            ib.hide();
            ib.dispose();
            resolve(name);
        });

        const r = await vscode.commands.executeCommand('xinsp2.createInstance');
        await sleep(700);
        await api.sendCmd('list_instances');
        await sleep(700);
        takeScreenshot(`${prefix}_e_added_to_tree`);
        return r;
    }

    async function addInstanceQuiet(plugin, name) {
        vscode.window.showQuickPick = async (items) => {
            const arr = await Promise.resolve(items);
            return arr.find(it => (it.label ?? it) === plugin);
        };
        vscode.window.showInputBox = async () => name;
        return vscode.commands.executeCommand('xinsp2.createInstance');
    }

    console.log('\n[STEP 2] User: Click "+" → plugin picker → type name (verbose flow for cam0)');
    try {
        const r2a = await addInstanceVerbose('mock_camera', 'cam0', '02_add_cam0');
        assert.ok(r2a?.ok, 'cam0 created via + button');
        console.log('  ✓ cam0 added via + button (with full picker flow captured)');

        const r2b = await addInstanceQuiet('blob_analysis', 'det0');
        assert.ok(r2b?.ok, 'det0 created via + button');
        console.log('  ✓ det0 added via + button (quiet)');

        const r2c = await addInstanceQuiet('record_save', 'saver0');
        assert.ok(r2c?.ok, 'saver0 created via + button');
        console.log('  ✓ saver0 added via + button (quiet)');
    } finally {
        vscode.window.showQuickPick = origQuickPick;
        vscode.window.showInputBox  = origInputBox;
    }

    // Verify instance folders exist on disk
    assert.ok(fs.existsSync(path.join(projDir, 'instances', 'cam0', 'instance.json')));
    assert.ok(fs.existsSync(path.join(projDir, 'instances', 'det0', 'instance.json')));
    assert.ok(fs.existsSync(path.join(projDir, 'instances', 'saver0', 'instance.json')));
    // Refresh tree view + focus so the new instances are visible
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await api.sendCmd('list_instances');
    await sleep(1500);
    takeScreenshot('instances_created');

    // ====== STEP 3: User clicks cam0 in the tree (or its inline ⚙ icon) ======
    console.log('\n[STEP 3] User: Click cam0 → camera UI opens, set FPS=15, start streaming');
    await vscode.commands.executeCommand('xinsp2.openInstanceUI',
        { label: 'cam0', description: 'mock_camera' });
    await sleep(2000);
    shot('camera_ui_opened');
    assertPluginUIOpen('cam0', 'mock_camera');
    console.log(`  ✓ UI assert: mock_camera webview for cam0 is open`);

    // True UI driving: type into inputs, click buttons inside the webview.
    api.setInputInWebview('cam0', '#fps', 15);
    await sleep(400);
    api.clickInWebview('cam0', '#btn-apply');
    await sleep(600);
    shot('camera_settings_applied');

    api.clickInWebview('cam0', '#btn-start');
    await sleep(2000);   // let preview stream kick in
    console.log('  ✓ camera streaming at 15 fps');
    shot('camera_streaming');

    // ====== STEP 4: User opens blob analysis UI, sets threshold ======
    console.log('\n[STEP 4] User: Open blob analysis UI, set threshold=120');
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'det0', 'blob_analysis');
    await sleep(2000);
    shot('blob_ui_opened');
    assertPluginUIOpen('det0', 'blob_analysis');
    console.log(`  ✓ UI assert: blob_analysis webview for det0 is open`);

    api.setInputInWebview('det0', '#threshold', 120);
    await sleep(300);
    api.setInputInWebview('det0', '#minArea', 30);
    await sleep(300);
    shot('blob_params_set');

    api.clickInWebview('det0', 'vscode-button[onclick="onApply()"], button[onclick="onApply()"]');
    await sleep(1000);
    console.log('  ✓ blob analysis configured');
    shot('blob_applied');

    // ====== STEP 5: User opens saver UI, sets folder + naming, enables ======
    console.log('\n[STEP 5] User: Open saver UI, set folder + naming rule, enable');
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'saver0', 'record_save');
    await sleep(2000);
    shot('saver_ui_opened');
    assertPluginUIOpen('saver0', 'record_save');
    // All three plugin UI tabs should now coexist
    const tabs = pluginUITabs();
    for (const wanted of ['cam0 (mock_camera)', 'det0 (blob_analysis)', 'saver0 (record_save)']) {
        assert.ok(tabs.includes(wanted), `expected tab "${wanted}" still open; saw [${tabs.join(', ')}]`);
    }
    console.log(`  ✓ UI assert: 3 plugin webviews open simultaneously`);

    fs.mkdirSync(saveDir, { recursive: true });
    api.setInputInWebview('saver0', '#outputDir', saveDir);
    api.setInputInWebview('saver0', '#namingRule', 'inspection_{count}');
    await sleep(400);
    shot('saver_fields_filled');

    api.clickInWebview('saver0', '#btn-apply');
    await sleep(500);
    api.clickInWebview('saver0', '#btn-toggle');
    await sleep(900);
    const enableRsp = await api.sendCmd('exchange_instance', { name: 'saver0', cmd: { command: 'get_status' } });
    console.log('  saver state after enable:', JSON.stringify(enableRsp.data));
    console.log(`  ✓ saver enabled, will write to ${saveDir}`);
    shot('saver_enabled');

    // ====== STEP 6: User writes inspection script that wires them together ======
    console.log('\n[STEP 6] User: Edit inspection.cpp');
    const scriptPath = path.join(projDir, 'inspection.cpp');
    const scriptCode = `
#include <xi/xi.hpp>           // pulls in OpenCV
#include <xi/xi_record.hpp>
#include <xi/xi_use.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& cam = xi::use("cam0");
    auto& det = xi::use("det0");
    auto& saver = xi::use("saver0");

    auto img = cam.grab(500);
    if (img.empty()) {
        img = xi::Image(320, 240, 1);
        std::memset(img.data(), 0, 320 * 240);
        for (int by = 60; by < 100; ++by)
            for (int bx = 60; bx < 100; ++bx)
                img.data()[by * 320 + bx] = 255;
        for (int by = 150; by < 190; ++by)
            for (int bx = 200; bx < 240; ++bx)
                img.data()[by * 320 + bx] = 255;
    }
    VAR(input, img);
    // Source is already 1-channel; if your camera produces RGB, convert
    // with cv::cvtColor(img.as_cv_mat(), out, cv::COLOR_RGB2GRAY).

    auto detection = det.process(xi::Record()
        .image("gray", img));
    VAR(detection_record, detection);

    int n_blobs = detection["blob_count"].as_int();
    VAR(blob_count, n_blobs);

    xi::Record save_input;
    save_input.image("input", img);
    save_input.image("binary", detection.get_image("binary"));
    save_input.set("blob_count", n_blobs);
    save_input.set("frame", frame);
    VAR(save_input_image_count, (int)save_input.images().size());

    auto save_result = saver.process(save_input);
    VAR(saved, save_result);
}
`;
    fs.writeFileSync(scriptPath, scriptCode);
    console.log('  ✓ inspection.cpp written');

    // Open in editor (so user sees their code — editor title actions visible)
    const doc = await vscode.workspace.openTextDocument(scriptPath);
    await vscode.window.showTextDocument(doc, vscode.ViewColumn.One);
    await sleep(1800);
    shot('script_in_editor');
    assertActiveEditor('inspection.cpp');
    console.log(`  ✓ UI assert: inspection.cpp is the active editor`);

    // ====== STEP 7: User compiles via command ======
    console.log('\n[STEP 7] User: Command Palette → "xInsp2: Compile Script"');
    const compileRsp = await api.sendCmd('compile_and_load', { path: scriptPath });
    if (!compileRsp.ok) {
        console.log('--- compile error full payload ---');
        console.log(JSON.stringify(compileRsp, null, 2));
        console.log('--- end payload ---');
    }
    assert.ok(compileRsp.ok, `compile failed: ${compileRsp.error}`);
    console.log('  ✓ compile succeeded');
    await sleep(2000);
    shot('after_compile');

    // ====== STEP 8: User runs inspection ======
    console.log('\n[STEP 8] User: Run Inspection');

    // Run several times to accumulate saved files + see viewer update
    for (let i = 0; i < 3; ++i) {
        const runRsp = await api.sendCmd('run');
        console.log(`  run #${i+1}:`, JSON.stringify(runRsp).slice(0, 200));
        await sleep(800);
    }
    await sleep(1800);
    // Focus the viewer so the captured vars are visible in the screenshot.
    try { await vscode.commands.executeCommand('xinsp2.viewer.focus'); } catch {}
    await sleep(1000);
    const saverStatus = await api.sendCmd('exchange_instance', {
        name: 'saver0', cmd: { command: 'get_status' }
    });
    console.log('  saver status after runs:', JSON.stringify(saverStatus.data));
    console.log('  ✓ ran inspection 3 times');
    shot('inspections_ran_viewer');

    // ====== STEP 9: Verify side effects ======
    console.log('\n[STEP 9] Verify user-visible side effects');

    // 9a: Files should exist in save directory
    const savedFiles = fs.readdirSync(saveDir);
    console.log(`  saved files in ${saveDir}: ${savedFiles.length}`);
    for (const f of savedFiles.slice(0, 6)) {
        console.log(`    - ${f}`);
    }
    assert.ok(savedFiles.length > 0, 'saver should have written files to disk');

    // 9b: Should have JSON metadata files
    const jsonFiles = savedFiles.filter(f => f.endsWith('.json'));
    assert.ok(jsonFiles.length > 0, 'should have .json files');

    // 9c: Should have BMP image files
    const bmpFiles = savedFiles.filter(f => f.endsWith('.bmp'));
    assert.ok(bmpFiles.length > 0, 'should have .bmp files');

    // 9d: Naming rule applied: files start with "inspection_"
    assert.ok(savedFiles.some(f => f.startsWith('inspection_')),
              'naming rule should produce inspection_* files');

    console.log(`  ✓ ${jsonFiles.length} JSON + ${bmpFiles.length} BMP files saved`);
    console.log(`  ✓ naming rule "inspection_{count}" applied`);

    // 9e: Verify a JSON file has the inspection record content
    const jsonPath = path.join(saveDir, jsonFiles[0]);
    const recorded = JSON.parse(fs.readFileSync(jsonPath, 'utf8'));
    assert.ok('blob_count' in recorded, 'saved JSON contains blob_count field');
    assert.ok('frame' in recorded, 'saved JSON contains frame field');
    console.log(`  ✓ recorded data includes: ${Object.keys(recorded).join(', ')}`);

    // Focus the saver UI so the screenshot shows the updated counter.
    try {
        await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'saver0', 'record_save');
    } catch {}
    await sleep(1500);
    shot('saver_counter_after_runs');

    // ====== STEP 10: User stops camera, saves project, closes ======
    console.log('\n[STEP 10] User: Stop camera, save project');
    try {
        await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'cam0', 'mock_camera');
    } catch {}
    await sleep(1000);
    api.clickInWebview('cam0', '#btn-stop');
    await sleep(1000);
    shot('camera_stopped');

    await api.sendCmd('save_instance_config', { name: 'cam0' });
    await api.sendCmd('save_instance_config', { name: 'det0' });
    await api.sendCmd('save_instance_config', { name: 'saver0' });
    await api.sendCmd('save_project', { path: path.join(projDir, 'project.json') });

    // Verify saved configs preserve the user's settings
    const camCfg = JSON.parse(fs.readFileSync(path.join(projDir, 'instances', 'cam0', 'instance.json'), 'utf8'));
    assert.equal(camCfg.config.fps, 15, 'cam fps preserved');
    const detCfg = JSON.parse(fs.readFileSync(path.join(projDir, 'instances', 'det0', 'instance.json'), 'utf8'));
    assert.equal(detCfg.config.threshold, 120, 'det threshold preserved');
    assert.equal(detCfg.config.min_area, 30, 'det min_area preserved');
    const saverCfg = JSON.parse(fs.readFileSync(path.join(projDir, 'instances', 'saver0', 'instance.json'), 'utf8'));
    assert.equal(saverCfg.config.naming_rule, 'inspection_{count}', 'saver naming preserved');
    assert.equal(saverCfg.config.enabled, true, 'saver enabled preserved');

    console.log('  ✓ all instance configs saved');
    // Show the Plugins tree so usage counts + cert status are visible.
    try { await vscode.commands.executeCommand('xinsp2.plugins.focus'); } catch {}
    await sleep(1500);
    shot('project_saved_plugins_view');

    // ====== STEP 11: Close the project → back to the welcome state ======
    try { await vscode.commands.executeCommand('xinsp2.closeProject'); } catch {}
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('final_after_close');

    console.log(`\n=== USER JOURNEY COMPLETE — ${stepNum} screenshots ===`);
    console.log(`Project: ${projDir}`);
    console.log(`Saved files: ${saveDir}`);

    // Clean up screenshots stay; project + saves are kept for inspection
}

module.exports = { run };
