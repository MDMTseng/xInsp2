// Launcher for the image viewer journey E2E.
// Walks project + plugin + instance + image viewer, then drives a
// scripted sequence of pan / zoom / fit / 1:1 / tool ops with one
// screenshot per step. See e2e/image_viewer_journey.cjs.
import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'examples');

process.env.XINSP2_E2E_SUITE = 'image_viewer_journey';

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [workspace, '--disable-extensions'],
    });
    console.log('Image Viewer Journey E2E PASSED');
} catch (err) {
    console.error('Image Viewer Journey E2E FAILED:', err);
    process.exit(1);
}
