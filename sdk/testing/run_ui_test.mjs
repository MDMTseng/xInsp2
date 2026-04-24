//
// run_ui_test.mjs — CLI launcher for plugin UI tests.
//
// Usage:
//   node <sdk>/testing/run_ui_test.mjs <plugin-folder>
//
// Requires xInsp2 to be installed locally. XINSP2_ROOT can point at the
// install; otherwise we auto-detect by climbing parents looking for
// vscode-extension/ + a built backend.
//

import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));

// ----- args -------------------------------------------------------------

const pluginFolder = resolve(process.argv[2] || process.cwd());
if (!existsSync(resolve(pluginFolder, 'plugin.json'))) {
    console.error(`no plugin.json at ${pluginFolder}`);
    process.exit(2);
}
if (!existsSync(resolve(pluginFolder, 'tests', 'test_ui.cjs'))) {
    console.error(`no tests/test_ui.cjs at ${pluginFolder}`);
    process.exit(2);
}

// ----- find XINSP2_ROOT -------------------------------------------------

function isXInsp2Root(p) {
    return existsSync(resolve(p, 'vscode-extension', 'package.json')) &&
           existsSync(resolve(p, 'backend', 'build', 'Release', 'xinsp-backend.exe'));
}

function autoDetectRoot() {
    // Walk up from the SDK folder, then sideways into xInsp2 siblings.
    let p = resolve(__dirname, '..');
    for (let i = 0; i < 6; ++i) {
        if (isXInsp2Root(p)) return p;
        const sib = resolve(p, 'xInsp2');
        if (isXInsp2Root(sib)) return sib;
        const parent = resolve(p, '..');
        if (parent === p) break;
        p = parent;
    }
    return null;
}

const xinsp2Root = process.env.XINSP2_ROOT
    ? resolve(process.env.XINSP2_ROOT)
    : autoDetectRoot();

if (!xinsp2Root || !isXInsp2Root(xinsp2Root)) {
    console.error('XINSP2_ROOT not found.');
    console.error('Set XINSP2_ROOT env var to your xInsp2 install (the dir containing backend/ and vscode-extension/).');
    process.exit(2);
}

const extensionDir   = resolve(xinsp2Root, 'vscode-extension');
const testElectron   = resolve(extensionDir, 'node_modules', '@vscode', 'test-electron', 'out', 'index.js');
const testRunner     = resolve(__dirname, 'e2e_entry.cjs');

if (!existsSync(testElectron)) {
    console.error(`@vscode/test-electron not installed at ${testElectron}`);
    console.error('Run "npm install" in the vscode-extension folder first.');
    process.exit(2);
}

console.log(`XINSP2_ROOT   = ${xinsp2Root}`);
console.log(`plugin folder = ${pluginFolder}`);
console.log(`extension dev = ${extensionDir}\n`);

// Tell the extension to scan this plugin's folder, and tell e2e_entry.cjs
// which plugin we're testing.
process.env.XINSP2_PLUGIN_FOLDER = pluginFolder;

// ----- spawn VS Code ----------------------------------------------------

const { runTests } = await import(pathToFileURL(testElectron).href);

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        // Open the plugin folder as the workspace + add it as a plugins dir
        // so the host scans/loads the plugin under test.
        launchArgs: [
            pluginFolder,
            '--disable-extensions',
        ],
        extensionTestsEnv: {
            XINSP2_PLUGIN_FOLDER: pluginFolder,
            // Backend will scan this dir in addition to the in-tree plugins/
            XINSP2_EXTRA_PLUGIN_DIRS: dirname(pluginFolder),
        },
    });
    console.log('\nplugin UI test PASSED');
} catch (err) {
    console.error('\nplugin UI test FAILED:', err && err.message ? err.message : err);
    process.exit(1);
}
