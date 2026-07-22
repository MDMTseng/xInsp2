// try_example.cjs — the shipped-example picker, end to end.
//
// This exists because the ux_states run that "covers" createSampleProject was
// passing without ever reaching it: the command guards on a connected backend,
// ux_states hits it while the backend is offline, and it returns early. A green
// screenshot proved nothing about the feature. So: connect first, then assert.
//
// What is asserted:
//   1. both commands are registered;
//   2. tryExample('mock_camera') opens a project whose instances are the ones
//      that example declares (cam + view) — i.e. it really opened THAT example,
//      not some generated stand-in;
//   3. it opened a COPY under the temp dir, not the shipped folder, and the copy
//      carries no build/run artifacts;
//   4. the shipped example is byte-untouched afterwards;
//   5. createSampleProject (no argument, no prompt) opens the cross-plugin
//      station, whose five instances are all present.

const vscode = require('vscode');
const path   = require('path');
const os     = require('os');
const fs     = require('fs');
const assert = require('assert');
const { sleep } = require('./journey_helpers.cjs');

const REPO = path.resolve(__dirname, '..', '..', '..');

function hashDir(dir) {
    // Cheap structural fingerprint: relative path + size for every file.
    const out = [];
    const walk = (d, rel) => {
        for (const e of fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => a.name < b.name ? -1 : 1)) {
            const p = path.join(d, e.name);
            if (e.isDirectory()) walk(p, rel + e.name + '/');
            else out.push(rel + e.name + ':' + fs.statSync(p).size);
        }
    };
    walk(dir, '');
    return out.join('\n');
}

// Read the instances OF THE PROJECT THAT IS ACTUALLY OPEN, from disk.
// Deliberately not list_instances: that command replies {} and pushes the list
// as a separate event, so polling its response yields nothing and the assertion
// would pass vacuously. project.json under api.projectFolder is the direct
// evidence for the claim being made here — "it opened THAT example".
function openedInstances(api) {
    const folder = api.projectFolder;
    assert.ok(folder, 'no project folder — nothing was opened');
    const pj = JSON.parse(fs.readFileSync(path.join(folder, 'project.json'), 'utf8'));
    return new Set((pj.instances || []).map((i) => i && i.name).filter(Boolean));
}

async function run() {
    const ext = vscode.extensions.all.find(e => e.id.includes('xinsp2') || e.id.includes('xception'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    assert.ok(await api.waitConnected(30000), 'backend must connect (the whole point of this suite)');

    // --- 1. commands registered ------------------------------------------
    const cmds = await vscode.commands.getCommands(true);
    assert.ok(cmds.includes('xinsp2.tryExample'), 'xinsp2.tryExample registered');
    assert.ok(cmds.includes('xinsp2.createSampleProject'), 'xinsp2.createSampleProject registered');
    console.log('[1] both commands registered');

    // --- 2/3/4. open one example by id ------------------------------------
    const shipped = path.join(REPO, 'toolbox', 'mock_camera', 'example');
    const before = hashDir(shipped);

    await vscode.commands.executeCommand('xinsp2.tryExample', 'mock_camera');
    await sleep(3000);

    const names = openedInstances(api);
    console.log('[2] instances in the opened project:', [...names].sort().join(', '));
    for (const want of ['cam', 'view']) {
        assert.ok(names.has(want),
            `instance '${want}' missing — this is not the mock_camera example (got ${[...names]})`);
    }

    const folder = api.projectFolder;
    console.log('[3] opened project folder:', folder);
    assert.ok(folder && folder.startsWith(os.tmpdir()),
        `expected a copy under ${os.tmpdir()}, got ${folder} — the shipped tree must not be opened in place`);
    assert.ok(fs.existsSync(path.join(folder, 'inspect.cpp')), 'copy has inspect.cpp');
    assert.ok(fs.existsSync(path.join(folder, 'README.md')), 'copy has README.md');
    // Artifacts the COPY must not carry. .xinsp_owner is deliberately absent from
    // this list: the backend writes that stamp itself when it opens a project, so
    // finding one here proves the open worked rather than that the copy was dirty.
    const stray = fs.readdirSync(folder).filter((f) => /^backend.*\.log$/.test(f));
    assert.strictEqual(stray.length, 0, `copy carried run artifacts: ${stray}`);
    assert.ok(!fs.existsSync(path.join(folder, 'build')), 'copy carried a build/ dir');

    assert.strictEqual(hashDir(shipped), before,
        'the SHIPPED example changed — opening one must never write to the repo copy');
    console.log('[4] shipped example untouched');

    // --- 5. the no-prompt entry point opens the station --------------------
    await vscode.commands.executeCommand('xinsp2.createSampleProject');
    await sleep(3000);
    const st = openedInstances(api);
    console.log('[5] instances after createSampleProject:', [...st].sort().join(', '));
    for (const want of ['cam', 'det', 'ring', 'saver', 'view']) {
        assert.ok(st.has(want),
            `station instance '${want}' missing — createSampleProject did not open toolbox/example (got ${[...st]})`);
    }

    console.log('\n=== TRY-EXAMPLE E2E COMPLETE ===');
}

module.exports = { run };
