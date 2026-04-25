// Launcher for the project-plugin journey E2E.
// Walks scaffold → typo + diagnostic → fix → medium template → instance →
// image viewer → export, with screenshots at every step.
import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'examples');

process.env.XINSP2_E2E_SUITE = 'project_plugin_journey';

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [workspace, '--disable-extensions'],
    });
    console.log('Project Plugin Journey E2E PASSED');
} catch (err) {
    console.error('Project Plugin Journey E2E FAILED:', err);
    process.exit(1);
}
