// Launcher: downloads VS Code if needed, runs the extension test suite.
// Usage: node test/runE2E.mjs

import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'examples');

// Use locally installed VS Code instead of downloading a fresh copy
const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [
            workspace,
            '--disable-extensions',
        ],
    });
    console.log('E2E tests passed');
} catch (err) {
    console.error('E2E tests failed:', err);
    process.exit(1);
}
