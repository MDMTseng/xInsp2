// Launcher for the user-journey E2E test.
// Drives the full project lifecycle (create → configure → script → run → save)
// entirely through user-facing VS Code commands.
import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'examples');

process.env.XINSP2_E2E_SUITE = 'user_journey';

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [workspace, '--disable-extensions'],
    });
    console.log('User Journey E2E PASSED');
} catch (err) {
    console.error('User Journey E2E FAILED:', err);
    process.exit(1);
}
