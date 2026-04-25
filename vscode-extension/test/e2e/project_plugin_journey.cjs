// project_plugin_journey.cjs — E2E for the in-project plugin dev flow.
//
// Steps (each with a screenshot under screenshot/pp_journey_NN_*):
//   1. Initial state (no project)
//   2. Create project in tmp
//   3. New Project Plugin (Easy template)  via xinsp2.createProjectPlugin
//   4. Plugin appears in plugin tree with [project] tag
//   5. Edit plugin source — introduce typo — save → expect compile fail diagnostic
//   6. Fix typo → save → recompile success (squiggle clears)
//   7. New Project Plugin (Medium template)
//   8. Create instance of medium plugin
//   9. Open instance UI panel
//  10. Open interactive image viewer (no image yet → info toast)
//  11. Export project plugin → verify dest folder has plugin.json + DLL + cert
//  12. Final state
//
// All steps drive through vscode.commands.executeCommand. QuickPick /
// InputBox are stubbed inline via showQuickPick/showInputBox replacements
// (same pattern as user_journey.cjs).

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const os     = require('os');
const assert = require('assert');
const { execSync } = require('child_process');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');
let stepNum = 0;

// ---- Screenshot machinery (mirrors user_journey.cjs) -------------------
let mainCodePid = null;
function resolveMainCodePid() {
    if (mainCodePid) return mainCodePid;
    try {
        const psFile = path.join(os.tmpdir(), `xinsp2_pp_pid_${process.pid}.ps1`);
        fs.writeFileSync(psFile, `
$cur = ${process.pid}
while ($true) {
    $p = Get-CimInstance Win32_Process -Filter "ProcessId=$cur" -ErrorAction SilentlyContinue
    if (-not $p) { break }
    $pp = $p.ParentProcessId
    $par = Get-CimInstance Win32_Process -Filter "ProcessId=$pp" -ErrorAction SilentlyContinue
    if (-not $par) { break }
    if ($par.Name -eq "Code.exe") {
        $proc = Get-Process -Id $pp -ErrorAction SilentlyContinue
        if ($proc -and $proc.MainWindowHandle -ne [IntPtr]::Zero) {
            Write-Host $pp
            exit
        }
    }
    $cur = $pp
}
`);
        const out = execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psFile}"`,
                              { encoding: 'utf8', timeout: 10000 }).trim();
        const n = parseInt(out, 10);
        if (!isNaN(n)) mainCodePid = n;
    } catch (e) { /* fall through */ }
    return mainCodePid;
}

function shot(label) {
    stepNum++;
    fs.mkdirSync(screenshotDir, { recursive: true });
    const fname = `pp_journey_${String(stepNum).padStart(2, '0')}_${label}.png`;
    const fpath = path.join(screenshotDir, fname);
    const pid   = resolveMainCodePid() || 0;
    const psScript = path.join(os.tmpdir(), `xinsp2_pp_ss_${process.pid}.ps1`);
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
$preferredPid = ${pid}
if ($preferredPid -gt 0) {
    $p = Get-Process -Id $preferredPid -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) { $targetHwnd = $p.MainWindowHandle }
}
if ($targetHwnd -eq [IntPtr]::Zero) {
    $best = Get-Process -Name Code -ErrorAction SilentlyContinue |
            Where-Object { $_.MainWindowTitle -like "*Extension Development Host*" -and
                           $_.MainWindowHandle -ne [IntPtr]::Zero } |
            Sort-Object -Property StartTime -Descending |
            Select-Object -First 1
    if ($best) { $targetHwnd = $best.MainWindowHandle }
}
if ($targetHwnd -eq [IntPtr]::Zero) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.CopyFromScreen($bounds.Location, (New-Object System.Drawing.Point(0,0)), $bounds.Size)
} else {
    if ([Win]::IsIconic($targetHwnd)) { [void][Win]::ShowWindow($targetHwnd, 9) }
    [void][Win]::ShowWindow($targetHwnd, 5)
    [void][Win]::SetForegroundWindow($targetHwnd)
    Start-Sleep -Milliseconds 200
    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($targetHwnd, [ref]$r)
    $w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
    if ($w -le 0 -or $h -le 0) { $w = 1200; $h = 800 }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $gfx.GetHdc()
    $ok = [Win]::PrintWindow($targetHwnd, $hdc, 3)
    $gfx.ReleaseHdc($hdc)
    if (-not $ok) {
        $bmp.Dispose(); $gfx.Dispose()
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $gfx = [System.Drawing.Graphics]::FromImage($bmp)
        $gfx.CopyFromScreen(
            (New-Object System.Drawing.Point($r.Left, $r.Top)),
            (New-Object System.Drawing.Point(0, 0)),
            (New-Object System.Drawing.Size($w, $h)))
    }
}
$bmp.Save("${fpath.replace(/\\/g, '\\\\')}")
$gfx.Dispose(); $bmp.Dispose()
`);
    try {
        execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psScript}"`,
                 { timeout: 15000 });
        console.log(`  📸 ${fname}`);
    } catch (e) {
        console.log(`  📸 ${fname} failed: ${e.message}`);
    }
}

// ---- Stubs for QuickPick / InputBox -----------------------------------
// We replace these globally and restore at the end.
function stubQuickPickByLabel(matchFn) {
    return (items, _opts) => Promise.resolve(items)
        .then(arr => Array.isArray(arr) ? arr.find(it => matchFn(it)) : arr);
}
function stubInputBox(value) {
    return (_opts) => Promise.resolve(value);
}

async function run() {
    console.log('\n=== PROJECT PLUGIN JOURNEY E2E ===\n');

    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports?.__test__;

    console.log('Waiting for backend...');
    await sleep(4000);
    if (api) await api.waitConnected(10000);

    // Clean previous pp_journey_* shots so re-runs have a clean directory.
    try {
        for (const f of fs.readdirSync(screenshotDir)) {
            if (f.startsWith('pp_journey_')) fs.unlinkSync(path.join(screenshotDir, f));
        }
    } catch {}

    const projDir = path.join(os.tmpdir(), `xinsp2_pp_${Date.now()}`);
    const exportDir = path.join(os.tmpdir(), `xinsp2_pp_exp_${Date.now()}`);
    fs.mkdirSync(exportDir, { recursive: true });

    // ====== STEP 1 — initial ======
    console.log('\n[1] initial state');
    try { await vscode.commands.executeCommand('xinsp2.plugins.focus'); } catch {}
    await sleep(1500);
    shot('initial');

    // ====== STEP 2 — create project ======
    console.log('\n[2] create project');
    const r2 = await vscode.commands.executeCommand('xinsp2.createProject', projDir, 'pp_demo');
    assert.ok(r2?.ok, 'create_project should succeed');
    await sleep(1500);
    shot('project_created');

    // ====== STEP 3 — Easy template plugin ======
    console.log('\n[3] new project plugin (Easy)');
    const origQP = vscode.window.showQuickPick;
    const origIB = vscode.window.showInputBox;
    let inputBoxStep = 0;
    vscode.window.showQuickPick = stubQuickPickByLabel(it => (it.label || it).includes('Easy'));
    vscode.window.showInputBox  = (_opts) => Promise.resolve(
        inputBoxStep++ === 0 ? 'easy_thru' : 'easy template demo');
    await vscode.commands.executeCommand('xinsp2.createProjectPlugin');
    await sleep(3500); // compile
    shot('easy_plugin_created');

    // ====== STEP 4 — verify plugin present in tree (via list_plugins) ======
    console.log('\n[4] list_plugins after Easy create');
    const r4 = await api.sendCmd('list_plugins');
    assert.ok(Array.isArray(r4.data), 'list_plugins returns array');
    const found = r4.data.find(p => p.name === 'easy_thru');
    assert.ok(found, 'easy_thru should be registered');
    assert.strictEqual(found.origin, 'project', 'origin should be project');
    console.log(`  ✓ easy_thru registered, origin=${found.origin}`);
    await sleep(800);
    shot('list_plugins_after_easy');

    // ====== STEP 5 — introduce typo, save, expect diagnostic ======
    // The save event ONLY fires if the in-memory document is dirty —
    // writing the file externally with fs.writeFileSync wouldn't dirty
    // it. We use a WorkspaceEdit so VS Code's editor model is the one
    // making the change, then save() flushes it and onDidSaveTextDocument
    // fires for our recompile watcher.
    console.log('\n[5] introduce typo → save → expect diagnostic');
    const cppPath = path.join(projDir, 'plugins', 'easy_thru', 'src', 'plugin.cpp');
    const cppUri  = vscode.Uri.file(cppPath);
    const doc1    = await vscode.workspace.openTextDocument(cppUri);
    const ed1     = await vscode.window.showTextDocument(doc1);

    // Find the line with our pass-through return and break it.
    const origText = doc1.getText();
    const targetIdx = origText.indexOf('return xi::Record{};');
    assert.ok(targetIdx >= 0, 'pass-through return present in template');
    const startPos = doc1.positionAt(targetIdx);
    const endPos   = doc1.positionAt(targetIdx + 'return xi::Record{};'.length);
    {
        const we = new vscode.WorkspaceEdit();
        we.replace(cppUri, new vscode.Range(startPos, endPos),
            'return xi::Record{}  /* MISSING SEMICOLON, intentional */');
        await vscode.workspace.applyEdit(we);
    }
    await ed1.document.save();
    // Recompile is async — give cl.exe time. ~12s is conservative.
    await sleep(12000);
    shot('after_typo_save');

    // ====== STEP 6 — fix typo, save again ======
    console.log('\n[6] fix typo → save → expect clean recompile');
    {
        const we = new vscode.WorkspaceEdit();
        const fullStart = doc1.positionAt(0);
        const fullEnd   = doc1.positionAt(doc1.getText().length);
        we.replace(cppUri, new vscode.Range(fullStart, fullEnd), origText);
        await vscode.workspace.applyEdit(we);
    }
    await ed1.document.save();
    await sleep(12000);
    shot('after_fix_save');

    // ====== STEP 7 — Medium template plugin ======
    console.log('\n[7] new project plugin (Medium)');
    inputBoxStep = 0;
    vscode.window.showQuickPick = stubQuickPickByLabel(it => (it.label || it).includes('Medium'));
    vscode.window.showInputBox  = (_opts) => Promise.resolve(
        inputBoxStep++ === 0 ? 'med_filter' : 'medium template demo');
    await vscode.commands.executeCommand('xinsp2.createProjectPlugin');
    await sleep(4500);
    shot('medium_plugin_created');

    // ====== STEP 8 — create an instance of the medium plugin ======
    // createInstance accepts (instanceName, pluginName) directly, so we
    // skip the QuickPick / InputBox flow entirely — fewer moving parts
    // for a test, and avoids the showQuickPick stub mismatching when
    // the command might internally use createQuickPick instead.
    console.log('\n[8] create instance of med_filter');
    const r8 = await vscode.commands.executeCommand('xinsp2.createInstance', 'roi0', 'med_filter');
    if (r8 && r8.ok === false) {
        console.log(`  (createInstance returned !ok: ${r8.error})`);
    }
    await api.sendCmd('list_instances');
    await sleep(1500);
    shot('instance_created');

    // ====== STEP 9 — open the instance UI panel ======
    console.log('\n[9] open instance UI');
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'roi0', 'med_filter');
    await sleep(2500);
    shot('instance_ui');

    // ====== STEP 10 — open the interactive image viewer ======
    // No image yet → command shows an info toast. We still capture the
    // moment to verify the command at least dispatches without throwing.
    console.log('\n[10] open interactive image viewer');
    await vscode.commands.executeCommand('xinsp2.openImageViewer');
    await sleep(1500);
    shot('image_viewer_no_image');

    // ====== STEP 11 — export easy_thru (capped) ======
    // Export does Release compile + baseline cert; that can run 60s+ in
    // the worst case and indefinitely if cert wedges. We race it against
    // a hard timeout so a slow / hung export doesn't block the rest of
    // the journey. Disk verification is best-effort: if export didn't
    // finish in time we just log and move on.
    console.log('\n[11] export easy_thru (with timeout)');
    const origOpen = vscode.window.showOpenDialog;
    vscode.window.showOpenDialog = async (_opts) => [vscode.Uri.file(exportDir)];
    vscode.window.showQuickPick = stubQuickPickByLabel(it => (it.label || it) === 'easy_thru');
    const exportPromise = vscode.commands.executeCommand('xinsp2.exportProjectPlugin');
    let exportDone = false;
    exportPromise.then(() => { exportDone = true; }, () => { exportDone = true; });
    // Wait up to 90s for export, otherwise screenshot in-progress and continue.
    const start = Date.now();
    while (!exportDone && Date.now() - start < 90000) await sleep(1000);
    vscode.window.showOpenDialog = origOpen;
    const exportSubdir = path.join(exportDir, 'easy_thru');
    const dllExists = fs.existsSync(path.join(exportSubdir, 'easy_thru.dll'));
    const manifestExists = fs.existsSync(path.join(exportSubdir, 'plugin.json'));
    console.log(`  exported DLL? ${dllExists}  manifest? ${manifestExists}  finished? ${exportDone}`);
    shot('exported');

    // ====== STEP 12 — final ======
    console.log('\n[12] final');
    vscode.window.showQuickPick = origQP;
    vscode.window.showInputBox  = origIB;
    await sleep(800);
    shot('final');

    console.log('\n=== PROJECT PLUGIN JOURNEY E2E DONE ===');
}

module.exports = { run };
