import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(__dirname, '..');
const testRunner   = resolve(__dirname, 'e2e', 'index.cjs');
const workspace    = resolve(__dirname, '..', '..', 'examples');

process.env.XINSP2_E2E_SUITE = 'ux_states';

const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';
const vscodeExecutablePath = existsSync(localVSCode) ? localVSCode : undefined;

try {
    await runTests({
        vscodeExecutablePath,
        extensionDevelopmentPath: extensionDir,
        extensionTestsPath: testRunner,
        launchArgs: [workspace, '--disable-extensions'],
        extensionTestsEnv: {
            // Pre-register the pre-built polished demo so state 6 can open it
            // without racing against the slower runtime rescan path.
            XINSP2_EXTRA_PLUGIN_DIRS: 'C:\\Users\\TRS001\\Documents\\workspace\\xInsp\\polished_demo',
        },
    });
    console.log('UX states captured');
} catch (err) {
    console.error('UX states FAILED:', err);
    process.exit(1);
}
