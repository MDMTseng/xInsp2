// Launcher: 3-node connected pipeline graph, rendered with edges + screenshot.
import { runTests } from '@vscode/test-electron';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
process.env.XINSP2_E2E_SUITE = 'graph_multi';
const localVSCode = 'C:\\Users\\TRS001\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe';

try {
    await runTests({
        vscodeExecutablePath: existsSync(localVSCode) ? localVSCode : undefined,
        extensionDevelopmentPath: resolve(__dirname, '..'),
        extensionTestsPath: resolve(__dirname, 'e2e', 'index.cjs'),
        launchArgs: [resolve(__dirname, '..', '..', 'examples'), '--disable-extensions'],
    });
    console.log('Graph Multi E2E PASSED');
} catch (err) {
    console.error('Graph Multi E2E FAILED:', err);
    process.exit(1);
}
