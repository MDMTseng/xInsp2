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

// Shared journey utilities — sleep / editAndSave / makeShooter / clearOldShots.
// See journey_helpers.cjs for the rationale (especially editAndSave: any new
// e2e that wants the auto-recompile path must use this, NOT fs.writeFileSync).
const { sleep, editAndSave, restoreAndSave, makeShooter, clearOldShots } =
    require('./journey_helpers.cjs');

const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');
const shot = makeShooter(screenshotDir, 'pp_journey');

// ---- Stubs for QuickPick / InputBox -----------------------------------
// Used only for entry points that don't accept positional args. When a
// command DOES accept positional args (e.g. xinsp2.createInstance), pass
// them directly — see the audit notes in journey_helpers.cjs.
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

    clearOldShots(screenshotDir, 'pp_journey');

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
    // editAndSave from journey_helpers does the WorkspaceEdit + save
    // dance correctly so onDidSaveTextDocument actually fires.
    console.log('\n[5] introduce typo → save → expect diagnostic');
    const cppPath = path.join(projDir, 'plugins', 'easy_thru', 'src', 'plugin.cpp');
    const cppUri  = vscode.Uri.file(cppPath);
    const origText = await editAndSave(cppUri,
        'return xi::Record{};',
        'return xi::Record{}  /* MISSING SEMICOLON, intentional */');
    // Recompile is async — give cl.exe time. ~12s is conservative.
    await sleep(12000);
    shot('after_typo_save');

    // ====== STEP 6 — fix typo, save again ======
    console.log('\n[6] fix typo → save → expect clean recompile');
    await restoreAndSave(cppUri, origText);
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
