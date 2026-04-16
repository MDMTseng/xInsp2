// Full pipeline E2E test — screenshots at every step.
//
// Steps:
// 1. Create new project
// 2. Load plugins
// 3. Create mock_camera instance
// 4. Create data_output instance
// 5. Open mock_camera config UI
// 6. Set FPS to 15
// 7. Start streaming
// 8. Wait 5 seconds (streaming with live preview)
// 9. Stop streaming
// 10. Final screenshot
//
// Each step takes a screenshot to xInsp2/screenshot/pipeline_NNN_<label>.png

const vscode = require('vscode');
const path   = require('path');
const assert = require('assert');
const fs     = require('fs');
const os     = require('os');
const { execSync } = require('child_process');

const sleep = (ms) => new Promise(r => setTimeout(r, ms));

const screenshotDir = path.resolve(__dirname, '..', '..', '..', 'screenshot');
let stepNum = 0;

function takeScreenshot(label) {
    stepNum++;
    fs.mkdirSync(screenshotDir, { recursive: true });
    const filename = `pipeline_${String(stepNum).padStart(2,'0')}_${label}.png`;
    const filepath = path.join(screenshotDir, filename);
    const psScript = path.join(os.tmpdir(), 'xinsp2_ss.ps1');
    fs.writeFileSync(psScript, `
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$pt = New-Object System.Drawing.Point(0, 0)
$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$gfx.CopyFromScreen($bounds.Location, $pt, $bounds.Size)
$bmp.Save("${filepath.replace(/\\/g, '\\\\')}")
$gfx.Dispose()
$bmp.Dispose()
`);
    try {
        execSync(`powershell -NoProfile -ExecutionPolicy Bypass -File "${psScript}"`, { timeout: 15000 });
        console.log(`[step ${stepNum}] screenshot: ${filename}`);
    } catch (e) {
        console.log(`[step ${stepNum}] screenshot failed: ${e.message}`);
    }
}

async function run() {
    console.log('[pipeline] starting full pipeline E2E test...\n');

    // Find and activate the extension
    let ext = vscode.extensions.all.find(e => e.id.includes('xinsp2'));
    if (!ext) throw new Error('xInsp2 extension not found');
    if (!ext.isActive) await ext.activate();
    const api = ext.exports?.__test__;

    // Wait for backend connection
    console.log('[pipeline] waiting for backend...');
    await sleep(4000);

    if (api) {
        const connected = await api.waitConnected(10000);
        console.log('[pipeline] connected:', connected);
    }

    const sendCmd = api?.sendCmd ?? (async () => ({ ok: false, error: 'no API' }));

    // Clean up old screenshots
    try {
        for (const f of fs.readdirSync(screenshotDir)) {
            if (f.startsWith('pipeline_')) fs.unlinkSync(path.join(screenshotDir, f));
        }
    } catch {}

    // --- Step 1: Create project ---
    console.log('\n[step 1] Creating project...');
    const projDir = path.join(os.tmpdir(), 'xinsp2_pipeline_test');
    try { fs.rmSync(projDir, { recursive: true }); } catch {}
    const createRsp = await sendCmd('create_project', { folder: projDir, name: 'pipeline_test' });
    assert.equal(createRsp.ok, true, 'create_project');
    console.log('[step 1] project created at', projDir);
    await sleep(1000);
    takeScreenshot('project_created');

    // --- Step 2: List plugins ---
    console.log('\n[step 2] Listing plugins...');
    const pluginsRsp = await sendCmd('list_plugins');
    assert.equal(pluginsRsp.ok, true, 'list_plugins');
    const pluginNames = pluginsRsp.data.map(p => p.name);
    console.log('[step 2] available plugins:', pluginNames.join(', '));
    takeScreenshot('plugins_listed');

    // --- Step 3: Create mock_camera instance ---
    console.log('\n[step 3] Creating mock_camera instance...');
    const camRsp = await vscode.commands.executeCommand('xinsp2.createInstance', 'cam0', 'mock_camera');
    console.log('[step 3] cam0 created');
    await sleep(1000);
    takeScreenshot('camera_instance_created');

    // --- Step 4: Create data_output instance ---
    console.log('\n[step 4] Creating data_output instance...');
    await sendCmd('load_plugin', { name: 'data_output' });
    const sinkRsp = await vscode.commands.executeCommand('xinsp2.createInstance', 'saver0', 'data_output');
    console.log('[step 4] saver0 created');
    await sleep(1000);
    takeScreenshot('datasink_instance_created');

    // --- Step 4b: Open example script to show CodeLens ---
    console.log('\n[step 4b] Opening defect_detection.cpp to show CodeLens...');
    const examplesDir = vscode.workspace.workspaceFolders?.[0]?.uri?.fsPath ?? '';
    const scriptFile = path.join(examplesDir, 'defect_detection.cpp');
    if (fs.existsSync(scriptFile)) {
        const doc = await vscode.workspace.openTextDocument(scriptFile);
        await vscode.window.showTextDocument(doc, vscode.ViewColumn.One);
        await sleep(3000);  // wait for CodeLens to appear
        takeScreenshot('codelens_visible');
    }

    // --- Step 5: Open mock_camera config UI ---
    console.log('\n[step 5] Opening camera config UI...');
    let camPanel = null;

    // Helper: send an exchange command and update the webview
    async function camExchange(cmd) {
        const r = await sendCmd('exchange_instance', { name: 'cam0', cmd });
        if (r.ok && r.data && camPanel) {
            const parsed = typeof r.data === 'string' ? JSON.parse(r.data) : r.data;
            camPanel.webview.postMessage({ type: 'status', ...parsed });
        }
        return r;
    }

    const uiRsp = await sendCmd('get_plugin_ui', { plugin: 'mock_camera' });
    if (uiRsp.ok) {
        const uiPath = uiRsp.data.ui_path;
        const htmlPath = path.join(uiPath, 'index.html');
        if (fs.existsSync(htmlPath)) {
            const html = fs.readFileSync(htmlPath, 'utf8');
            camPanel = vscode.window.createWebviewPanel(
                'xinsp2.pluginUI', 'cam0 (mock_camera)',
                vscode.ViewColumn.Two, { enableScripts: true }
            );
            camPanel.webview.html = html;
            camPanel.webview.onDidReceiveMessage(async (msg) => {
                if (msg.type === 'exchange' && msg.cmd) {
                    await camExchange(msg.cmd);
                } else if (msg.type === 'request_preview') {
                    const r = await sendCmd('preview_instance', { name: 'cam0' }).catch(() => null);
                    // Register the gid → panel mapping so binary frames route correctly
                    if (r && r.ok && r.data && r.data.gid && api?.registerPreviewGid) {
                        api.registerPreviewGid(r.data.gid, camPanel);
                    }
                }
            });
            // Register panel so extension routes preview frames to it
            if (api?.registerPluginPanel) {
                api.registerPluginPanel('cam0', camPanel);
            }
            // Send initial status
            await camExchange({ command: 'get_status' });
            console.log('[step 5] camera UI panel opened');
        } else {
            console.log('[step 5] UI html not found:', htmlPath);
        }
    } else {
        console.log('[step 5] get_plugin_ui failed:', uiRsp.error);
    }
    await sleep(2000);
    takeScreenshot('camera_ui_opened');

    // --- Step 6: Set FPS to 15 ---
    console.log('\n[step 6] Setting FPS to 15...');
    const fpsRsp = await camExchange({ command: 'set_fps', value: 15 });
    console.log('[step 6] set_fps result:', JSON.stringify(fpsRsp.data));
    await sleep(1000);
    takeScreenshot('fps_set_to_15');

    // --- Step 7: Start streaming ---
    console.log('\n[step 7] Starting streaming...');
    const startRsp = await camExchange({ command: 'start' });
    console.log('[step 7] camera started:', JSON.stringify(startRsp.data));
    await sendCmd('start', { fps: 5 });
    await sleep(2000);
    takeScreenshot('streaming_started');

    // --- Step 8: Wait 5 seconds while streaming ---
    console.log('\n[step 8] Streaming for 5 seconds...');
    await sleep(2000);
    takeScreenshot('streaming_2s');
    await sleep(3000);
    takeScreenshot('streaming_5s');

    // --- Step 9: Stop streaming ---
    console.log('\n[step 9] Stopping streaming...');
    await sendCmd('stop');
    const stopRsp = await camExchange({ command: 'stop' });
    console.log('[step 9] camera stopped:', JSON.stringify(stopRsp.data));
    await sleep(1000);
    takeScreenshot('streaming_stopped');

    // --- Step 9b: Open blob_analysis UI ---
    console.log('\n[step 9b] Creating blob_analysis instance and opening UI...');
    await sendCmd('load_plugin', { name: 'blob_analysis' });
    await sendCmd('create_instance', { name: 'detector0', plugin: 'blob_analysis' });

    // Open blob analysis UI
    const blobUiRsp = await sendCmd('get_plugin_ui', { plugin: 'blob_analysis' });
    let blobPanel = null;
    if (blobUiRsp.ok) {
        const htmlPath = path.join(blobUiRsp.data.ui_path, 'index.html');
        if (fs.existsSync(htmlPath)) {
            const html = fs.readFileSync(htmlPath, 'utf8');
            blobPanel = vscode.window.createWebviewPanel(
                'xinsp2.pluginUI', 'detector0 (blob_analysis)',
                vscode.ViewColumn.Two, { enableScripts: true }
            );
            blobPanel.webview.html = html;
            if (api?.registerPluginPanel) api.registerPluginPanel('detector0', blobPanel);

            // Send test blob data to the UI so we can see contour rendering
            await sleep(1000);

            // Simulate a process result with test blobs
            blobPanel.webview.postMessage({
                type: 'process_result',
                blob_count: 3,
                blobs: [
                    { area: 247, cx: 85.2, cy: 63.1, min_x: 72, min_y: 50, max_x: 98, max_y: 76, contour_points: 52,
                      contour: Array.from({length: 40}, (_, i) => ({
                          x: 85 + Math.round(13 * Math.cos(i * Math.PI * 2 / 40)),
                          y: 63 + Math.round(13 * Math.sin(i * Math.PI * 2 / 40))
                      }))
                    },
                    { area: 183, cx: 200.5, cy: 150.8, min_x: 185, min_y: 138, max_x: 216, max_y: 164, contour_points: 38,
                      contour: Array.from({length: 30}, (_, i) => ({
                          x: 200 + Math.round(15 * Math.cos(i * Math.PI * 2 / 30)),
                          y: 151 + Math.round(12 * Math.sin(i * Math.PI * 2 / 30))
                      }))
                    },
                    { area: 95, cx: 280.0, cy: 200.0, min_x: 270, min_y: 192, max_x: 290, max_y: 208, contour_points: 24,
                      contour: Array.from({length: 20}, (_, i) => ({
                          x: 280 + Math.round(10 * Math.cos(i * Math.PI * 2 / 20)),
                          y: 200 + Math.round(8 * Math.sin(i * Math.PI * 2 / 20))
                      }))
                    }
                ]
            });

            // Also send a preview image as background
            // Request a frame from the camera as the background
            const prevRsp = await sendCmd('preview_instance', { name: 'cam0' }).catch(() => null);
            if (prevRsp && prevRsp.ok && prevRsp.data && prevRsp.data.gid && api?.registerPreviewGid) {
                api.registerPreviewGid(prevRsp.data.gid, blobPanel);
            }
            await sleep(2000);
        }
    }
    takeScreenshot('blob_analysis_ui');

    // --- Step 10: Save instance configs ---
    console.log('\n[step 10] Saving instance configs...');
    await sendCmd('save_instance_config', { name: 'cam0' });
    await sendCmd('save_instance_config', { name: 'saver0' });
    takeScreenshot('configs_saved');

    // Verify project structure
    const projJson = JSON.parse(fs.readFileSync(path.join(projDir, 'project.json'), 'utf8'));
    console.log('\n[pipeline] project.json:', JSON.stringify(projJson, null, 2));
    assert.ok(projJson.instances.length >= 2, 'project has 2 instances');

    const camConfig = JSON.parse(fs.readFileSync(path.join(projDir, 'instances', 'cam0', 'instance.json'), 'utf8'));
    console.log('[pipeline] cam0 config:', JSON.stringify(camConfig));
    assert.equal(camConfig.plugin, 'mock_camera');

    console.log(`\n[pipeline] ALL STEPS COMPLETE — ${stepNum} screenshots in ${screenshotDir}`);

    // Cleanup
    try { fs.rmSync(projDir, { recursive: true }); } catch {}
}

module.exports = { run };
