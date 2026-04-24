// Capture screenshots of the extension in each UX state:
//   1. No project (welcome view for Instances)
//   2. Plugins tree loaded
//   3. Project just created (instances empty but tree visible)
//   4. Project with 2 instances
//   5. After adding a plugin folder
//
// Runs inside the Extension Host (launched by runUxStates.mjs).

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const os     = require('os');
const { execSync } = require('child_process');

const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');
let stepNum = 0;
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

function shot(label) {
    stepNum++;
    fs.mkdirSync(screenshotDir, { recursive: true });
    const fname = `ux_${String(stepNum).padStart(2,'0')}_${label}.png`;
    const fpath = path.join(screenshotDir, fname);
    const ps = path.join(os.tmpdir(), `xi_ux_ss_${process.pid}.ps1`);
    // Use PrintWindow on the current extension-host's VS Code main window.
    // GetForegroundWindow won't work if another app stole focus; so we
    // look up our own process's main window handle (the Extension Host's
    // parent is the same Code.exe process hosting this Node).
    // Use PrintWindow to capture VS Code directly from its own buffer —
    // works even if another window is on top. PW_RENDERFULLCONTENT (0x02)
    // is needed because VS Code uses Chromium GPU rendering.
    fs.writeFileSync(ps, `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$targetHwnd = [IntPtr]::Zero
# Pick the most-recent Code.exe with a non-empty title (the extension host's
# window). StartTime descending → the test-run VS Code comes first.
$procs = Get-Process -Name Code -ErrorAction SilentlyContinue |
         Where-Object { $_.MainWindowTitle -ne "" -and $_.MainWindowHandle -ne [IntPtr]::Zero } |
         Sort-Object StartTime -Descending
if ($procs) { $targetHwnd = $procs[0].MainWindowHandle }

if ($targetHwnd -eq [IntPtr]::Zero) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($bounds.Location, (New-Object System.Drawing.Point(0,0)), $bounds.Size)
} else {
    if ([Win]::IsIconic($targetHwnd)) { [void][Win]::ShowWindow($targetHwnd, 9) }
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($targetHwnd, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { $w = 1200; $h = 800 }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    [void][Win]::PrintWindow($targetHwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
    $gfx.ReleaseHdc($hdc)
}
$bmp.Save("${fpath.replace(/\\/g, '\\\\')}")
$gfx.Dispose(); $bmp.Dispose()
`);
    try {
        execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${ps}"`, { timeout: 20000 });
        console.log(`  📸 ${fname}`);
    } catch (e) {
        console.log(`  📸 ${fname} failed: ${e.message}`);
    }
}

async function run() {
    console.log('\n=== UX states capture ===\n');
    const ext = vscode.extensions.all.find(e => e.id.toLowerCase().includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;

    // Clear prior ux_* screenshots
    try {
        for (const f of fs.readdirSync(screenshotDir)) {
            if (f.startsWith('ux_')) fs.unlinkSync(path.join(screenshotDir, f));
        }
    } catch {}

    // Wait for backend
    await sleep(4500);
    if (api) await api.waitConnected(10000);

    // --- STATE 1: connected, no project (welcome view shown)
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('1_no_project_welcome');

    // --- STATE 2: Plugins view (filled because backend scanned built-in dir)
    try { await vscode.commands.executeCommand('xinsp2.plugins.focus'); } catch {}
    await sleep(1500);
    shot('2_plugins_view');

    // --- STATE 3: project created, empty instance tree (secondary welcome)
    const projDir = path.join(os.tmpdir(), `xinsp2_ux_${Date.now()}`);
    await vscode.commands.executeCommand('xinsp2.createProject', projDir, 'ux_demo');
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1800);
    shot('3_project_created_empty_welcome');

    // --- STATE 4: project with a mock_camera instance (title-bar buttons active)
    await vscode.commands.executeCommand('xinsp2.createInstance', 'cam0', 'mock_camera');
    await vscode.commands.executeCommand('xinsp2.createInstance', 'det0', 'blob_analysis');
    await sleep(1000);
    await api.sendCmd('list_instances');
    await sleep(1500);
    shot('4_project_with_instances');

    // --- STATE 5: Plugins view after instances (should show "N in use")
    try { await vscode.commands.executeCommand('xinsp2.plugins.focus'); } catch {}
    await sleep(1500);
    shot('5_plugins_with_usage');

    // --- STATE 6: Open a plugin's webview UI to show the vscode-elements polish.
    // The polished demo plugin is pre-registered via XINSP2_EXTRA_PLUGIN_DIRS
    // in the launcher so the backend discovers it at startup.
    try {
        await vscode.commands.executeCommand('xinsp2.createInstance', 'polish0', 'polished');
        await sleep(600);
        await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'polish0', 'polished');
    } catch (e) { console.log('  (polished demo not available): ' + e.message); }
    await sleep(3500);
    shot('6_plugin_webview_polished');

    // --- STATE 7: Recent projects QuickPick
    vscode.commands.executeCommand('xinsp2.openRecent').catch(() => {});
    await sleep(900);
    shot('7_recent_projects_picker');
    try { await vscode.commands.executeCommand('workbench.action.closeQuickOpen'); } catch {}
    await sleep(400);

    // --- STATE 8: Cert drill-down panel for a plugin
    try {
        await vscode.commands.executeCommand('xinsp2.showPluginCert',
            { label: 'mock_camera' });
    } catch {}
    await sleep(2200);
    shot('8_cert_detail_panel');

    // --- STATE 9: Close project → back to the empty welcome
    try { await vscode.commands.executeCommand('xinsp2.closeProject'); } catch {}
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('9_after_close_project');

    // --- STATE 10: mock_camera UI (migrated to @vscode-elements)
    const projDir2 = path.join(os.tmpdir(), `xinsp2_ux10_${Date.now()}`);
    await vscode.commands.executeCommand('xinsp2.createProject', projDir2, 'ux_cam');
    await vscode.commands.executeCommand('xinsp2.createInstance', 'cam0', 'mock_camera');
    await sleep(500);
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'cam0', 'mock_camera');
    await sleep(2500);
    shot('10_mock_camera_polished');

    // --- STATE 11: record_save UI (migrated)
    await vscode.commands.executeCommand('xinsp2.createInstance', 'saver0', 'record_save');
    await sleep(400);
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'saver0', 'record_save');
    await sleep(2000);
    shot('11_record_save_polished');

    // --- STATE 12: blob_analysis UI (migrated)
    await vscode.commands.executeCommand('xinsp2.createInstance', 'det0', 'blob_analysis');
    await sleep(400);
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'det0', 'blob_analysis');
    await sleep(2000);
    shot('12_blob_analysis_polished');

    // --- STATE 13: Editor title actions — open inspection.cpp + screenshot
    // the Compile/Run icons in the editor title bar.
    try {
        const scriptPath = path.join(projDir2, 'inspection.cpp');
        const doc = await vscode.workspace.openTextDocument(scriptPath);
        await vscode.window.showTextDocument(doc, vscode.ViewColumn.Three);
    } catch {}
    await sleep(1800);
    shot('13_editor_title_actions');

    // --- STATE 14: Instance rename prompt
    const renamePromise = vscode.commands.executeCommand('xinsp2.renameInstance',
        { label: 'det0' });
    await sleep(1200);
    shot('14_rename_instance_prompt');
    try { await vscode.commands.executeCommand('workbench.action.closeQuickOpen'); } catch {}
    await renamePromise.then(() => {}, () => {});
    await sleep(300);

    // --- STATE 15: Backend-offline state — status bar should flip to warning
    try { await vscode.commands.executeCommand('xinsp2.restartBackend'); } catch {}
    await sleep(300);   // brief window before reconnect
    shot('15_backend_restarting');

    // Let backend reconnect for the remaining states.
    await sleep(5000);
    await api.waitConnected(10000);

    // --- STATE 16: Updated no-project welcome with "Try a Sample Project"
    try { await vscode.commands.executeCommand('xinsp2.closeProject'); } catch {}
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('16_welcome_with_sample_cta');

    // --- STATE 17: Sample project (mock_camera + blob_analysis preconfigured)
    try { await vscode.commands.executeCommand('xinsp2.createSampleProject'); } catch {}
    await sleep(2500);
    shot('17_sample_project_running');

    // --- STATE 18: json_source redesigned UI
    await vscode.commands.executeCommand('xinsp2.createInstance', 'cfg0', 'json_source');
    await sleep(500);
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'cfg0', 'json_source');
    await sleep(2500);
    shot('18_json_source_polished');

    console.log('\n=== UX states captured ===');
}

module.exports = { run };
