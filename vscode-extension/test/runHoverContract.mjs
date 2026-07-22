// Launcher for the use("…") plugin I/O contract hover E2E.
// Spawns VS Code with the dev extension, lets the managed backend connect,
// opens examples/blob_tracker, and asserts the hover over xi::use("det")
// renders the plugin's copyable Inputs/Outputs/Params contract.
import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'qa');

process.env.XINSP2_E2E_SUITE = 'hover_contract';

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [workspace, '--disable-extensions'],
    });
    console.log('Hover Contract E2E PASSED');
} catch (err) {
    console.error('Hover Contract E2E FAILED:', err);
    process.exit(1);
}
