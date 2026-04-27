// image_viewer_journey.cjs — e2e walkthrough that combines plugin
// creation with discrete pan/zoom operations on the interactive
// image viewer. One screenshot per step under screenshot/iv_journey_NN_*.
//
// Steps:
//   1. Initial state (no project)
//   2. Create project
//   3. Create Easy template plugin
//   4. Create instance of the plugin
//   5. Open instance UI panel
//   6. Open image viewer with synthetic 16x16 JPEG
//   7. fit (reset to "fit-to-stage")
//   8. zoom in at center (factor 1.5)
//   9. zoom in at center again (factor 2.0)  → ~3x total
//  10. zoom in at corner (sx=20, sy=20, factor 1.5)
//  11. pan right (dx=+60)
//  12. pan down  (dy=+40)
//  13. fit (reset)
//  14. 1:1 zoom
//  15. tool → point
//  16. tool → area
//  17. invariant selftest (anchor + clamp)  — same as project_plugin_journey
//  18. final
//
// Each pan/zoom step uses xinsp2.imageViewer.applyOp which posts a
// message into the webview, performs the op, and echoes the
// resulting transform back. The op is the only thing the e2e can
// drive — real wheel/mouse events don't reach a webview from the
// extension host.

const vscode = require('vscode');
const path   = require('path');
const fs     = require('fs');
const os     = require('os');
const assert = require('assert');

const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');
const shot = makeShooter(screenshotDir, 'iv_journey');

function stubQuickPickByLabel(matchFn) {
    return (items, _opts) => Promise.resolve(items)
        .then(arr => Array.isArray(arr) ? arr.find(it => matchFn(it)) : arr);
}

// Minimal valid baseline JPEG, 16×16, decodable by createImageBitmap.
const TINY_JPEG =
    '/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIx' +
    'wcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAAQABABAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL' +
    '/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJi' +
    'coKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ip' +
    'qrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/APvSiiiigD//2Q==';

async function applyOp(op) {
    const r = await vscode.commands.executeCommand('xinsp2.imageViewer.applyOp', op);
    if (!r || r.ok === false) {
        console.log(`  applyOp FAILED: ${JSON.stringify(r)}`);
    } else {
        console.log(`  applyOp ok kind=${r.kind} scale=${r.scale.toFixed(3)} pan=(${r.panX.toFixed(1)},${r.panY.toFixed(1)}) tool=${r.tool}`);
    }
    return r;
}

async function run() {
    console.log('\n=== IMAGE VIEWER JOURNEY E2E ===\n');

    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports?.__test__;

    console.log('Waiting for backend...');
    await sleep(4000);
    if (api) await api.waitConnected(10000);

    clearOldShots(screenshotDir, 'iv_journey');

    const projDir = path.join(os.tmpdir(), `xinsp2_iv_${Date.now()}`);

    // ====== 1 — initial ======
    console.log('\n[1] initial state');
    try { await vscode.commands.executeCommand('xinsp2.plugins.focus'); } catch {}
    await sleep(1500);
    shot('initial');

    // ====== 2 — create project ======
    console.log('\n[2] create project');
    const r2 = await vscode.commands.executeCommand('xinsp2.createProject', projDir, 'iv_demo');
    assert.ok(r2?.ok, 'create_project should succeed');
    await sleep(1500);
    shot('project_created');

    // ====== 3 — Easy template plugin ======
    console.log('\n[3] new project plugin (Easy)');
    const origQP = vscode.window.showQuickPick;
    const origIB = vscode.window.showInputBox;
    let inputBoxStep = 0;
    vscode.window.showQuickPick = stubQuickPickByLabel(it => (it.label || it).includes('Easy'));
    vscode.window.showInputBox  = (_opts) => Promise.resolve(
        inputBoxStep++ === 0 ? 'iv_thru' : 'image viewer demo');
    await vscode.commands.executeCommand('xinsp2.createProjectPlugin');
    await sleep(3500);
    vscode.window.showQuickPick = origQP;
    vscode.window.showInputBox  = origIB;
    shot('easy_plugin_created');

    // ====== 4 — create instance ======
    console.log('\n[4] create instance');
    const r4 = await vscode.commands.executeCommand('xinsp2.createInstance', 'roi0', 'iv_thru');
    if (r4 && r4.ok === false) console.log(`  (createInstance returned !ok: ${r4.error})`);
    await sleep(1500);
    shot('instance_created');

    // ====== 5 — instance UI ======
    console.log('\n[5] open instance UI');
    await vscode.commands.executeCommand('xinsp2.openInstanceUI', 'roi0', 'iv_thru');
    await sleep(2500);
    shot('instance_ui');

    // ====== 6 — open image viewer with synthetic JPEG ======
    console.log('\n[6] open image viewer with 16x16 synthetic JPEG');
    await vscode.commands.executeCommand('xinsp2.openImageViewer', {
        jpeg: TINY_JPEG, name: 'journey-16x16', width: 16, height: 16,
    });
    await sleep(2000);
    shot('viewer_opened');

    // ====== 7 — fit ======
    console.log('\n[7] op: fit');
    const r7 = await applyOp({ kind: 'fit' });
    assert.ok(r7.ok, 'fit should succeed');
    const fitScale = r7.scale;
    await sleep(500);
    shot('op_fit');

    // ====== 8 — zoom in at center, factor 1.5 ======
    console.log('\n[8] op: zoom-in center x1.5');
    const r8 = await applyOp({ kind: 'zoom', factor: 1.5 });
    assert.ok(Math.abs(r8.scale - fitScale * 1.5) < 1e-6,
              `expected scale=${fitScale*1.5}, got ${r8.scale}`);
    await sleep(500);
    shot('op_zoom_in_1');

    // ====== 9 — zoom in again x2 ======
    console.log('\n[9] op: zoom-in center x2.0');
    const r9 = await applyOp({ kind: 'zoom', factor: 2.0 });
    assert.ok(r9.scale > r8.scale, 'scale should grow');
    await sleep(500);
    shot('op_zoom_in_2');

    // ====== 10 — zoom in at corner anchor ======
    console.log('\n[10] op: zoom anchored at (sx=20,sy=20) x1.5');
    const r10 = await applyOp({ kind: 'zoom', sx: 20, sy: 20, factor: 1.5 });
    assert.ok(r10.ok);
    await sleep(500);
    shot('op_zoom_corner');

    // ====== 11 — pan right ======
    console.log('\n[11] op: pan dx=+60');
    const px0 = r10.panX;
    const r11 = await applyOp({ kind: 'pan', dx: 60 });
    assert.ok(Math.abs(r11.panX - (px0 + 60)) < 1e-6,
              `expected panX=${px0+60}, got ${r11.panX}`);
    await sleep(500);
    shot('op_pan_right');

    // ====== 12 — pan down ======
    console.log('\n[12] op: pan dy=+40');
    const py0 = r11.panY;
    const r12 = await applyOp({ kind: 'pan', dy: 40 });
    assert.ok(Math.abs(r12.panY - (py0 + 40)) < 1e-6);
    await sleep(500);
    shot('op_pan_down');

    // ====== 13 — fit (reset) ======
    console.log('\n[13] op: fit (reset)');
    const r13 = await applyOp({ kind: 'fit' });
    assert.ok(Math.abs(r13.scale - fitScale) < 1e-6, 'fit should restore initial scale');
    await sleep(500);
    shot('op_fit_reset');

    // ====== 14 — 1:1 ======
    console.log('\n[14] op: 1:1');
    const r14 = await applyOp({ kind: 'oneToOne' });
    assert.ok(Math.abs(r14.scale - 1) < 1e-6, '1:1 must give scale=1');
    await sleep(500);
    shot('op_1to1');

    // ====== 15 — tool: point ======
    console.log('\n[15] op: tool=point');
    const r15 = await applyOp({ kind: 'tool', tool: 'point' });
    assert.strictEqual(r15.tool, 'point');
    await sleep(500);
    shot('tool_point');

    // ====== 16 — tool: area ======
    console.log('\n[16] op: tool=area');
    const r16 = await applyOp({ kind: 'tool', tool: 'area' });
    assert.strictEqual(r16.tool, 'area');
    await sleep(500);
    shot('tool_area');

    // ====== 17 — selftest (invariant pan + zoom math) ======
    console.log('\n[17] selftest: anchor + pan + clamp invariants');
    // tool back to move so the selftest's pan op doesn't trip area mode.
    await applyOp({ kind: 'tool', tool: 'move' });
    const selftest = await vscode.commands.executeCommand('xinsp2.imageViewer.runSelftest');
    console.log(`  selftest ok=${selftest.ok}`);
    for (const s of (selftest.steps || [])) {
        console.log(`    ${s.ok ? '✓' : '✗'} ${s.label}: ${s.detail}`);
    }
    assert.ok(selftest.ok, 'pan/zoom invariants must hold');
    await sleep(500);
    shot('selftest_done');

    // ====== 18 — final ======
    console.log('\n[18] final');
    await sleep(800);
    shot('final');

    console.log('\n=== IMAGE VIEWER JOURNEY E2E DONE ===');
}

module.exports = { run };
