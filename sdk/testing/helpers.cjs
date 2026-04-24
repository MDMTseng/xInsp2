//
// helpers.cjs — the `h` object passed into plugin UI tests.
//
// Thin facade over the xInsp2 extension's __test__ API plus the VS Code
// commands a real user would invoke. Safe to call from both the CLI
// launcher (@vscode/test-electron session) and the live-session command.
//

const vscode   = require('vscode');
const path     = require('path');
const fs       = require('fs');
const os       = require('os');
const { execSync } = require('child_process');

async function makeHelpers(pluginFolder, opts = {}) {
    // 1. Find + activate the extension, grab the __test__ API
    const ext = vscode.extensions.all.find(e => e.id.toLowerCase().includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found among loaded extensions');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports && ext.exports.__test__;
    if (!api) throw new Error('xInsp2 extension does not expose __test__ API');
    await api.waitConnected(15000);

    // 2. Screenshot dir — default to <plugin>/tests/screenshots/
    const shotDir = opts.screenshotDir || path.join(pluginFolder, 'tests', 'screenshots');
    try { fs.mkdirSync(shotDir, { recursive: true }); } catch {}
    let shotIdx = 0;

    // 3. State tracked across calls
    let lastStatus = null;
    const failures = [];
    const passes   = [];

    const h = {
        api,
        pluginFolder,
        sleep: (ms) => new Promise(r => setTimeout(r, ms)),
        tmp:   () => path.join(os.tmpdir(),
            `xinsp2_pluginui_${Date.now()}_${Math.random().toString(36).slice(2, 8)}`),

        async createProject(folder, name) {
            const r = await vscode.commands.executeCommand('xinsp2.createProject', folder, name);
            if (!r || !r.ok) throw new Error(`createProject(${folder}) failed: ${JSON.stringify(r)}`);
            return r;
        },

        async addInstance(name, plugin) {
            const r = await vscode.commands.executeCommand('xinsp2.createInstance', name, plugin);
            if (!r || !r.ok) throw new Error(`addInstance(${name}, ${plugin}) failed: ${JSON.stringify(r)}`);
            return r;
        },

        async openUI(name, plugin) {
            await vscode.commands.executeCommand('xinsp2.openInstanceUI', name, plugin);
            // Let the webview mount and ask for initial status
            await h.sleep(1500);
        },

        click:    (instance, selector)          => api.clickInWebview(instance, selector),
        setInput: (instance, selector, value)   => api.setInputInWebview(instance, selector, value),

        async sendCmd(instance, cmd) {
            const r = await api.sendCmd('exchange_instance', { name: instance, cmd });
            if (r && r.ok && r.data != null) {
                try {
                    lastStatus = typeof r.data === 'string' ? JSON.parse(r.data) : r.data;
                } catch { /* leave lastStatus as-is on parse error */ }
            }
            return r;
        },

        async getStatus(instance) {
            return h.sendCmd(instance, { command: 'get_status' });
        },

        get lastStatus() { return lastStatus; },

        async run() {
            return vscode.commands.executeCommand('xinsp2.run');
        },

        // Screenshot via PowerShell — same mechanism the main E2E uses.
        shot(label) {
            shotIdx++;
            const fname = `${String(shotIdx).padStart(2, '0')}_${label}.png`;
            const fpath = path.join(shotDir, fname);
            const ps = path.join(os.tmpdir(), `xi_pluginui_ss_${process.pid}.ps1`);
            fs.writeFileSync(ps, `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$pt = New-Object System.Drawing.Point(0, 0)
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($bounds.Location, $pt, $bounds.Size)
$bmp.Save("${fpath.replace(/\\/g, '\\\\')}")
$gfx.Dispose(); $bmp.Dispose()
`);
            try {
                execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${ps}"`, { timeout: 15000 });
                console.log(`  📸 ${fname}`);
            } catch (e) {
                console.log(`  📸 ${fname} failed: ${e.message}`);
            }
            return fpath;
        },

        expect(cond, msg) {
            const m = msg || 'assertion';
            if (cond) { passes.push(m); console.log(`  ✓ ${m}`); }
            else      { failures.push(m); console.log(`  ✗ ${m}`); }
        },

        expectEq(actual, expected, msg) {
            const label = msg || 'expectEq';
            if (actual === expected ||
                JSON.stringify(actual) === JSON.stringify(expected)) {
                passes.push(label);
                console.log(`  ✓ ${label}`);
            } else {
                const m = `${label}: got ${JSON.stringify(actual)}, expected ${JSON.stringify(expected)}`;
                failures.push(m);
                console.log(`  ✗ ${m}`);
            }
        },

        get failures() { return failures.slice(); },
        get passes()   { return passes.slice(); },
    };

    return h;
}

module.exports = { makeHelpers };
