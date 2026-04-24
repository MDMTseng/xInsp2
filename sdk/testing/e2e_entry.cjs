//
// e2e_entry.cjs — runs inside the VS Code Extension Host when the CLI
// launcher spawns VS Code via @vscode/test-electron. Loads the target
// plugin's tests/test_ui.cjs, hands it an `h` helpers object, runs.
//

const path = require('path');
const fs   = require('fs');
const { makeHelpers } = require('./helpers.cjs');

async function run() {
    const pluginFolder = process.env.XINSP2_PLUGIN_FOLDER;
    if (!pluginFolder) throw new Error('XINSP2_PLUGIN_FOLDER env var not set by the launcher');

    const testFile = path.join(pluginFolder, 'tests', 'test_ui.cjs');
    if (!fs.existsSync(testFile)) throw new Error(`no test file at ${testFile}`);

    console.log(`\n=== xInsp2 plugin UI test ===`);
    console.log(`plugin folder: ${pluginFolder}`);
    console.log(`test file:     ${testFile}\n`);

    // Give the extension + backend a beat to come up before we query it
    await new Promise(r => setTimeout(r, 4000));

    const h = await makeHelpers(pluginFolder);

    const mod = require(testFile);
    const runFn = (typeof mod === 'function') ? mod :
                  (mod && typeof mod.run === 'function') ? mod.run : null;
    if (!runFn) throw new Error(`${testFile} must export run(h) (or default-export a function)`);

    let thrown = null;
    try { await runFn(h); }
    catch (e) { thrown = e; }

    const nFail = h.failures.length;
    const nPass = h.passes.length;
    console.log(`\n${nPass} passed, ${nFail} failed`);
    if (thrown) console.log(`threw: ${thrown.stack || thrown}`);

    if (thrown) throw thrown;
    if (nFail > 0) throw new Error(`${nFail} assertion failure(s)`);
}

module.exports = { run };
