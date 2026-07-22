// show_icon.cjs — open a project, focus the Instances view, and screenshot so
// the "Open Pipeline Graph" title-bar icon is visible (where to click it).

const vscode = require('vscode');
const path   = require('path');
const assert = require('assert');
const { sleep, makeShooter, clearOldShots } = require('./journey_helpers.cjs');

const slash = (s) => s.split('\\').join('/');
const REPO  = path.resolve(__dirname, '..', '..', '..');
const PROJECT = path.join(REPO, 'qa', 'graph_demo');

const screenshotDir = path.join(REPO, 'screenshot');
const shot = makeShooter(screenshotDir, 'show_icon');

async function run() {
    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    assert.ok(await api.waitConnected(20000), 'backend must connect');
    clearOldShots(screenshotDir, 'show_icon');

    // Open a project so the title-bar buttons (gated on connected && hasProject)
    // appear, and compile so everything is live.
    await vscode.commands.executeCommand('xinsp2.openProject', slash(PROJECT));
    await api.sendCmd('compile_and_load', { path: slash(path.join(PROJECT, 'inspect.cpp')) });
    await api.sendCmd('list_instances');
    await sleep(1500);

    // Reveal the xInsp2 sidebar + the Instances view so its title bar shows.
    try { await vscode.commands.executeCommand('workbench.view.extension.xinsp2'); } catch {}
    try { await vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
    await sleep(1500);
    shot('instances_view');

    // Also prove the command is registered + actually open the graph for context.
    const cmds = await vscode.commands.getCommands(true);
    assert.ok(cmds.includes('xinsp2.openPipelineGraph'), 'openPipelineGraph command registered');
    await vscode.commands.executeCommand('xinsp2.openPipelineGraph');
    await sleep(1200);
    shot('graph_opened');
    console.log('OK — screenshots written');
}

module.exports = { run };
