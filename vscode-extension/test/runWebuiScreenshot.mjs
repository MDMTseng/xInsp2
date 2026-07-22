// Launcher for the plugin-webui open + screenshot E2E.
// Spawns VS Code with the dev extension, opens examples/blob_tracker, opens
// the "det" instance webui, asserts the panel tab exists, and PrintWindow's
// the dev-host into screenshot/webui_*.png for a human spot-check.
import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'qa');

process.env.XINSP2_E2E_SUITE = 'webui_screenshot';

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [workspace, '--disable-extensions'],
    });
    console.log('Webui Screenshot E2E PASSED');
} catch (err) {
    console.error('Webui Screenshot E2E FAILED:', err);
    process.exit(1);
}
