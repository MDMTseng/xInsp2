// journey_helpers.cjs — patterns shared across e2e journeys.
//
// Why this file exists: the project-plugin journey audit caught two
// gotchas that any new journey will hit, so cement them here:
//
//   1. fs.writeFileSync + document.save() does NOT fire
//      onDidSaveTextDocument because the in-memory document isn't
//      dirty. Use vscode.WorkspaceEdit (editAndSave below) so save()
//      flushes a real change and watchers fire.
//
//   2. Stubbing showQuickPick / showInputBox is brittle when the
//      command might use createQuickPick / createInputBox internally.
//      If a command accepts the picked values as positional args, pass
//      them directly and skip the picker entirely.
//
// Plus a couple of small utilities the journeys both need: the
// PowerShell-PrintWindow screenshot routine resolves the right Code.exe
// PID by walking up the EHE process chain.

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const os     = require('os');
const { execSync } = require('child_process');

// Sleep without pulling in a promise-utils dep.
const sleep = (ms) => new Promise(r => setTimeout(r, ms));

// ---- Editor mutations that ACTUALLY fire save events ------------------
// Any journey that wants the auto-recompile / squiggle path to fire
// must go through here, not fs.writeFileSync.

/**
 * Replace the first occurrence of `find` in the document at `uri` with
 * `replace`, then save the document. Returns the original text so the
 * caller can restore it later. If `find` is null, replaces the whole
 * document with `replace`.
 */
async function editAndSave(uri, find, replace) {
    const doc = await vscode.workspace.openTextDocument(uri);
    const ed  = await vscode.window.showTextDocument(doc);
    const original = doc.getText();

    const we = new vscode.WorkspaceEdit();
    if (find === null) {
        const fullStart = doc.positionAt(0);
        const fullEnd   = doc.positionAt(original.length);
        we.replace(uri, new vscode.Range(fullStart, fullEnd), replace);
    } else {
        const idx = original.indexOf(find);
        if (idx < 0) throw new Error(`editAndSave: '${find}' not in ${uri.fsPath}`);
        const start = doc.positionAt(idx);
        const end   = doc.positionAt(idx + find.length);
        we.replace(uri, new vscode.Range(start, end), replace);
    }
    await vscode.workspace.applyEdit(we);
    await ed.document.save();
    return original;
}

/** Restore a doc to its original text, then save. */
async function restoreAndSave(uri, original) {
    return editAndSave(uri, null, original);
}

// ---- Screenshots -----------------------------------------------------
// Find the specific Code.exe that owns THIS Extension Host (walk up the
// process tree from process.pid). PrintWindow it directly so unrelated
// VS Code instances on the user's desktop don't get captured.

let mainCodePid = null;
function resolveMainCodePid() {
    if (mainCodePid) return mainCodePid;
    try {
        const psFile = path.join(os.tmpdir(), `xinsp2_jhelpers_pid_${process.pid}.ps1`);
        // $cur, NOT $pid — $pid is read-only in PowerShell.
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
    } catch (e) { /* fall back to title-match in capture */ }
    return mainCodePid;
}

/**
 * Make a screenshooter bound to a specific output directory + filename
 * prefix. The returned function takes a label and writes
 * `<dir>/<prefix>_NN_<label>.png`. Numbering is shared across calls so
 * sequence is preserved.
 */
function makeShooter(outDir, prefix) {
    let n = 0;
    return function shoot(label) {
        n++;
        fs.mkdirSync(outDir, { recursive: true });
        const fname = `${prefix}_${String(n).padStart(2, '0')}_${label}.png`;
        const fpath = path.join(outDir, fname);
        const pid = resolveMainCodePid() || 0;
        const psScript = path.join(os.tmpdir(), `xinsp2_jhelpers_ss_${process.pid}.ps1`);
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
    };
}

/** Wipe `<outDir>/<prefix>_*.png` so re-runs don't accumulate stale shots. */
function clearOldShots(outDir, prefix) {
    try {
        for (const f of fs.readdirSync(outDir)) {
            if (f.startsWith(prefix + '_') && f.endsWith('.png')) {
                fs.unlinkSync(path.join(outDir, f));
            }
        }
    } catch { /* dir may not exist yet */ }
}

module.exports = {
    sleep,
    editAndSave,
    restoreAndSave,
    makeShooter,
    clearOldShots,
};
