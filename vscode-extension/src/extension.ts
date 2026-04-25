import * as vscode from 'vscode';
import * as path from 'path';
import { spawn, ChildProcess } from 'child_process';
import { WsClient } from './wsClient';
import { InstanceTreeProvider } from './instanceTree';
import { ViewerProvider } from './viewerProvider';
import { InstanceCodeLensProvider } from './instanceCodeLens';
import { PluginTreeProvider, PluginInfo } from './pluginTree';
import { PREVIEW_HEADER_SIZE } from './protocol';

let backend: ChildProcess | null = null;
// Auto-respawn state. `intendedRunning` is true while the extension wants
// the backend up — set to false on `dispose()` and on the explicit
// shutdown command, so a clean exit doesn't trigger a respawn.
let intendedRunning = false;
// Sliding window of recent respawn timestamps (ms epoch) for rate limit.
const recentRespawnsMs: number[] = [];
const MAX_RESPAWNS_PER_MINUTE = 5;
// Last project we know was opened. Set by handlers below; replayed on
// every successful (re)connect so a respawned backend lands the user
// back on their working tree.
let lastProjectFolder: string | null = null;
// Set inside activate(); used by xinsp2.restartBackend so manual restarts
// reuse the auto-respawn-aware spawn helper.
let spawnAndWatchHandle: (() => void) | null = null;

// Effective auto-respawn flag, computed as:
//   project.json's `auto_respawn` (when a project is open and field set)
//     overrides the workspace setting `xinsp2.autoRespawn` (default true).
// Recomputed on every open_project and on every workspace-config change.
let autoRespawnEnabled = true;
function recomputeAutoRespawn() {
    const cfg = vscode.workspace.getConfiguration('xinsp2');
    let next = cfg.get<boolean>('autoRespawn', true);
    if (lastProjectFolder) {
        try {
            const p = require('path').join(lastProjectFolder, 'project.json');
            if (require('fs').existsSync(p)) {
                const j = JSON.parse(require('fs').readFileSync(p, 'utf8'));
                if (typeof j.auto_respawn === 'boolean') next = j.auto_respawn;
            }
        } catch { /* ignore parse errors — keep workspace default */ }
    }
    autoRespawnEnabled = next;
}
let client: WsClient | null = null;
let cmdId = 1;
const nextId = () => cmdId++;

// Webview HTML for the cert drill-down panel.
function certPanelHtml(
    pluginName: string, folder: string, plugin: any, certJson: any
): string {
    const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;');
    const certState = !plugin?.cert?.present ? 'missing' :
                       plugin.cert?.valid     ? 'valid'   : 'stale';
    const statusColor = certState === 'valid' ? '#4caf50' :
                         certState === 'stale' ? '#ff9800' : '#f44336';
    const statusLabel = certState === 'valid' ? 'Valid' :
                         certState === 'stale' ? 'Stale (DLL changed)' : 'Missing';
    const tests = Array.isArray(certJson?.tests_passed) ? certJson.tests_passed : [];
    const testRows = tests.map((t: string) =>
        `<div class="test"><span class="check">✓</span> ${esc(t)}</div>`
    ).join('');
    return `<!doctype html>
<html><head><meta charset="utf-8"><style>
body { font-family: var(--vscode-font-family, sans-serif); background: var(--vscode-editor-background); color: var(--vscode-foreground); padding: 24px; line-height: 1.5; }
h1 { margin: 0 0 4px; font-size: 20px; display:flex; align-items:center; gap:10px; }
.status-pill { display:inline-block; padding: 2px 10px; border-radius: 10px; font-size: 11px; font-weight: 600; background: ${statusColor}; color: #fff; letter-spacing: 0.3px; }
.meta { color: var(--vscode-descriptionForeground); font-size: 12px; margin-bottom: 24px; }
.card { background: var(--vscode-editor-background); border: 1px solid var(--vscode-widget-border, rgba(255,255,255,0.08)); border-radius: 6px; padding: 16px 20px; margin-bottom: 14px; }
.card-title { font-size: 11px; text-transform: uppercase; letter-spacing: 0.6px; color: var(--vscode-descriptionForeground); margin: 0 0 12px; font-weight: 600; }
.grid { display: grid; grid-template-columns: 180px 1fr; gap: 8px 18px; font-size: 13px; }
.grid .k { color: var(--vscode-descriptionForeground); }
.grid .v { font-family: var(--vscode-editor-font-family, monospace); word-break: break-all; }
.test { padding: 4px 0; font-family: var(--vscode-editor-font-family, monospace); font-size: 12px; }
.check { color: #4caf50; margin-right: 8px; }
#recert-result { margin-top: 12px; font-size: 12px; }
.actions { margin-top: 18px; }
</style></head><body>
<h1>${esc(pluginName)} <span class="status-pill">${statusLabel}</span></h1>
<div class="meta">Plugin folder: ${esc(folder)}</div>

<div class="card">
    <div class="card-title">Summary</div>
    <div class="grid">
        <div class="k">Name</div>             <div class="v">${esc(pluginName)}</div>
        <div class="k">Description</div>      <div class="v">${esc(plugin?.description || '—')}</div>
        <div class="k">Loaded</div>           <div class="v">${plugin?.loaded ? 'yes' : 'no'}</div>
        <div class="k">Has UI</div>           <div class="v">${plugin?.has_ui ? 'yes' : 'no'}</div>
    </div>
</div>

<div class="card">
    <div class="card-title">Certificate (cert.json)</div>
    ${certJson ? `
    <div class="grid">
        <div class="k">Certified at</div>     <div class="v">${esc(certJson.certified_at || '—')}</div>
        <div class="k">Baseline version</div> <div class="v">${certJson.baseline_version ?? '—'}</div>
        <div class="k">Duration</div>         <div class="v">${certJson.duration_ms != null ? certJson.duration_ms.toFixed(1) + ' ms' : '—'}</div>
        <div class="k">DLL size</div>         <div class="v">${certJson.dll_size != null ? certJson.dll_size.toLocaleString() + ' bytes' : '—'}</div>
        <div class="k">DLL mtime</div>        <div class="v">${certJson.dll_mtime != null ? certJson.dll_mtime : '—'}</div>
    </div>
    <div style="margin-top: 16px;">
        <div class="card-title" style="margin-bottom: 8px">Tests passed (${tests.length})</div>
        ${testRows || '<div class="meta">None recorded.</div>'}
    </div>
    ` : `<div class="meta">No cert.json present. The host will re-certify on next load; or click below to do it now.</div>`}
    <div class="actions">
        <button id="btn-recert" style="padding: 6px 14px; border: none; border-radius: 3px; background: #2196f3; color: #fff; cursor: pointer;">
            Re-certify now
        </button>
        <span id="recert-result"></span>
    </div>
</div>

<script>
const vscode = acquireVsCodeApi();
document.getElementById('btn-recert').addEventListener('click', () => {
    document.getElementById('recert-result').textContent = 'Running baseline…';
    vscode.postMessage({ type: 'recert' });
});
window.addEventListener('message', (e) => {
    const m = e.data;
    if (m?.type === 'recert_result') {
        const msg = m.passed
            ? \`✓ \${m.pass_count} tests passed (\${m.total_ms?.toFixed?.(0) ?? '?'} ms)\`
            : \`⚠ \${m.fail_count} failed: \` + (m.failures || []).map(f => f.name).join(', ');
        document.getElementById('recert-result').textContent = msg;
    }
});
</script>
</body></html>`;
}

function findBackendExe(context: vscode.ExtensionContext): string {
    // Explicit override always wins.
    const cfg = vscode.workspace.getConfiguration('xinsp2');
    const explicit = (cfg.get<string>('backendExe', '') || '').trim();
    if (explicit) return explicit;

    const candidates = [
        // Dev tree: vscode-extension/ is sibling of backend/
        path.join(context.extensionPath, '..', 'backend', 'build', 'Release', 'xinsp-backend.exe'),
        // Packaged: exe shipped next to extension
        path.join(context.extensionPath, 'backend', 'xinsp-backend.exe'),
    ];

    // Also check workspace folders.
    for (const wf of vscode.workspace.workspaceFolders ?? []) {
        candidates.push(path.join(wf.uri.fsPath, 'backend', 'build', 'Release', 'xinsp-backend.exe'));
        // Walk up from workspace to find xInsp2 root
        candidates.push(path.join(wf.uri.fsPath, '..', 'backend', 'build', 'Release', 'xinsp-backend.exe'));
        candidates.push(path.join(wf.uri.fsPath, '..', '..', 'backend', 'build', 'Release', 'xinsp-backend.exe'));
    }

    const fs = require('fs');
    for (const c of candidates) {
        const resolved = path.resolve(c);
        if (fs.existsSync(resolved)) {
            return resolved;
        }
    }
    return 'xinsp-backend.exe';
}

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('xinsp2');
    const port = config.get<number>('backendPort', 7823);
    const autoStart = config.get<boolean>('autoStartBackend', true);
    const extraPluginDirs = config.get<string[]>('extraPluginDirs', []);
    const remoteUrl = (config.get<string>('remoteUrl', '') || '').trim();
    const authSecret = (config.get<string>('authSecret', '') || '').trim();
    // Remote mode: skip spawning a local backend, connect to the given URL.
    // Combine with authSecret to drive a backend started with --auth.
    const isRemote = remoteUrl.length > 0;
    const wsUrl = isRemote ? remoteUrl : `ws://127.0.0.1:${port}`;
    const output = vscode.window.createOutputChannel('xInsp2');
    // Initialise auto-respawn flag from workspace config; the project.json
    // override picks up later when a project opens.
    recomputeAutoRespawn();
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration(e => {
            if (e.affectsConfiguration('xinsp2.autoRespawn')) recomputeAutoRespawn();
        })
    );

    // ---- State context keys ------------------------------------------
    // Every menu contribution gates off these. Welcome views also use them.
    const setCtx = (key: string, value: any) =>
        vscode.commands.executeCommand('setContext', `xinsp2.${key}`, value);
    setCtx('connected', false);
    setCtx('hasProject', false);
    setCtx('running', false);
    setCtx('hasPlugins', false);
    setCtx('hasInstances', false);

    // Backend health indicator (leftmost). Toggles between connected + disconnected.
    const healthStatus = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 101);
    healthStatus.command = 'xinsp2.restartBackend';
    context.subscriptions.push(healthStatus);
    const updateHealthStatus = (connected: boolean) => {
        if (connected) {
            healthStatus.text = '$(zap) xInsp2';
            healthStatus.tooltip = 'xInsp2 backend connected. Click to restart.';
            healthStatus.backgroundColor = undefined;
        } else {
            healthStatus.text = '$(debug-disconnect) xInsp2 · offline';
            healthStatus.tooltip = 'xInsp2 backend is not reachable. Click to restart.';
            healthStatus.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
        }
        healthStatus.show();
    };
    updateHealthStatus(false);

    // Persistent project label in the status bar (click to switch/close).
    const projectStatus = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    projectStatus.command = 'xinsp2.projectStatusClicked';
    context.subscriptions.push(projectStatus);
    let currentProjectName: string | undefined;
    let currentProjectPath: string | undefined;
    const updateProjectStatus = () => {
        if (currentProjectName) {
            projectStatus.text = `$(folder-active) xInsp2: ${currentProjectName}`;
            projectStatus.tooltip = `xInsp2 project: ${currentProjectPath || ''}\nClick to switch or close.`;
            projectStatus.show();
        } else {
            projectStatus.hide();
        }
    };
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.projectStatusClicked', async () => {
            const pick = await vscode.window.showQuickPick([
                { label: '$(folder-opened) Open different project…', action: 'open' },
                { label: '$(close) Close current project', action: 'close' },
                { label: '$(history) Recent projects…',   action: 'recent' },
            ], { placeHolder: `Current: ${currentProjectName || '(none)'}` });
            if (!pick) return;
            if ((pick as any).action === 'open')   vscode.commands.executeCommand('xinsp2.openProject');
            if ((pick as any).action === 'close')  vscode.commands.executeCommand('xinsp2.closeProject');
            if ((pick as any).action === 'recent') vscode.commands.executeCommand('xinsp2.openRecent');
        })
    );

    // ---- Recent projects (globalState) -------------------------------
    const RECENT_KEY = 'xinsp2.recentProjects';
    const MAX_RECENT = 10;
    type Recent = { path: string; name: string; timestamp: number };
    const getRecent = (): Recent[] => context.globalState.get<Recent[]>(RECENT_KEY, []);
    const addRecent = (folder: string, name: string) => {
        const list = getRecent().filter(r => path.resolve(r.path).toLowerCase() !== path.resolve(folder).toLowerCase());
        list.unshift({ path: folder, name, timestamp: Date.now() });
        context.globalState.update(RECENT_KEY, list.slice(0, MAX_RECENT));
    };

    // ---- Tree views --------------------------------------------------
    const treeProvider = new InstanceTreeProvider();
    const instancesView = vscode.window.createTreeView('xinsp2.instances', { treeDataProvider: treeProvider });

    const pluginTreeProvider = new PluginTreeProvider();
    pluginTreeProvider.setRemovableFolders(extraPluginDirs);
    const pluginsView = vscode.window.createTreeView('xinsp2.plugins', { treeDataProvider: pluginTreeProvider });

    // Activity-bar / view badge — surfaces connection state visually.
    function setViewBadge(connected: boolean, instanceCount: number, pluginCount: number) {
        // Disconnected: red "!" badge.
        if (!connected) {
            instancesView.badge = { tooltip: 'xInsp2 backend offline', value: 1 };
            pluginsView.badge   = undefined;
            return;
        }
        instancesView.badge = instanceCount > 0
            ? { tooltip: `${instanceCount} instance(s)`, value: instanceCount }
            : undefined;
        pluginsView.badge = pluginCount > 0
            ? { tooltip: `${pluginCount} plugin(s) discovered`, value: pluginCount }
            : undefined;
    }
    let lastInstanceCount = 0;
    let lastPluginCount = 0;
    let lastConnected = false;

    // CodeLens for instance/param declarations
    const codeLensProvider = new InstanceCodeLensProvider();
    context.subscriptions.push(
        vscode.languages.registerCodeLensProvider(
            { language: 'cpp', scheme: 'file' },
            codeLensProvider
        )
    );

    // Param focus command (for CodeLens click on params)
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.focusParam', (paramName: string) => {
            vscode.window.showInformationMessage(`xInsp2: param "${paramName}" — use the sidebar to tune`);
            try { vscode.commands.executeCommand('xinsp2.instances.focus'); } catch {}
        })
    );

    // Preview variable command (for CodeLens click on VAR lines)
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.previewVar', (varName: string) => {
            viewerProvider.highlightVar(varName);
            try { vscode.commands.executeCommand('xinsp2.viewer.focus'); } catch {}
        })
    );

    // Viewer webview (side panel)
    const viewerProvider = new ViewerProvider(context.extensionUri);
    context.subscriptions.push(
        vscode.window.registerWebviewViewProvider('xinsp2.viewer', viewerProvider, {
            webviewOptions: { retainContextWhenHidden: true },
        })
    );

    // WS client — local spawn by default, or remote when xinsp2.remoteUrl is set.
    output.appendLine(isRemote
        ? `[xinsp2] remote mode → ${wsUrl}${authSecret ? ' (auth)' : ''}`
        : `[xinsp2] local mode → ws://127.0.0.1:${port}`);
    client = new WsClient({ url: wsUrl, authSecret: authSecret || undefined });

    client.on('open', () => {
        output.appendLine('[xinsp2] connected to backend');
        vscode.window.setStatusBarMessage('xInsp2: connected', 3000);
        setCtx('connected', true);
        updateHealthStatus(true);
        lastConnected = true;
        setViewBadge(true, lastInstanceCount, lastPluginCount);
        // Pull plugin list for the Plugins view as soon as we're up.
        sendCmd('list_plugins').then((r: any) => {
            if (r?.ok && Array.isArray(r.data)) {
                pluginTreeProvider.update(r.data as PluginInfo[]);
                setCtx('hasPlugins', r.data.length > 0);
            }
        }).catch(() => {});
        // Surface any unread crash reports from previous sessions. The
        // notification names the faulty module so the user knows whether
        // their script, a plugin, or the backend itself was at fault.
        sendCmd('crash_reports').then((r: any) => {
            const reports: any[] = r?.data?.reports || [];
            if (reports.length === 0) return;
            const newest = reports[0]?.report;
            const blame = newest?.exception?.module || 'unknown';
            const code  = newest?.exception?.name || newest?.exception?.code || 'crash';
            const cmd   = newest?.context?.last_cmd || '';
            const inst  = newest?.context?.last_instance;
            const detail = inst ? ` while talking to instance "${inst}"` : (cmd ? ` during ${cmd}` : '');
            const msg = `xInsp2 recovered: previous session crashed (${code}) in ${blame}${detail}.`
                      + ` ${reports.length} report(s) saved.`;
            output.appendLine('[xinsp2] ' + msg);
            for (const rep of reports.slice(0, 3)) {
                output.appendLine('  ' + JSON.stringify(rep.report?.exception) + ' ctx=' + JSON.stringify(rep.report?.context));
            }
            vscode.window.showWarningMessage(msg, 'Open dump folder', 'Dismiss & clear')
                .then(choice => {
                    if (choice === 'Open dump folder') {
                        const path = require('path');
                        const dir = path.join(require('os').tmpdir(), 'xinsp2', 'crashdumps');
                        vscode.env.openExternal(vscode.Uri.file(dir));
                    } else if (choice === 'Dismiss & clear') {
                        sendCmd('clear_crash_reports');
                    }
                });
        }).catch(() => {});
        // Auto-respawn recovery: if we know what project the user was in
        // before the backend died, re-open it so they land back where
        // they were. Skipped on the very first connect (lastProjectFolder
        // is still null) and after a clean closeProject (set to null).
        if (lastProjectFolder) {
            output.appendLine(`[xinsp2] restoring project: ${lastProjectFolder}`);
            sendCmd('open_project', { folder: lastProjectFolder }).then((r: any) => {
                if (r?.ok) {
                    setCtx('hasProject', true);
                    currentProjectName = r.data?.name || path.basename(lastProjectFolder!);
                    currentProjectPath = lastProjectFolder!;
                    setCtx('hasInstances', (r.data?.instances?.length ?? 0) > 0);
                    sendCmd('list_instances');
                    treeProvider.setProjectOpen(true);
                    updateProjectStatus();
                    vscode.window.setStatusBarMessage('xInsp2: project restored', 3000);
                } else {
                    output.appendLine(`[xinsp2] could not restore project: ${r?.error || 'unknown'}`);
                }
            }).catch((e) => output.appendLine(`[xinsp2] restore error: ${e?.message || e}`));
        }
    });

    client.on('close', () => {
        output.appendLine('[xinsp2] disconnected');
        setCtx('connected', false);
        setCtx('hasProject', false);
        setCtx('hasInstances', false);
        currentProjectName = undefined;
        currentProjectPath = undefined;
        updateProjectStatus();
        updateHealthStatus(false);
        treeProvider.setProjectOpen(false);
        lastConnected = false;
        setViewBadge(false, 0, 0);
    });

    client.on('json', (msg: any) => {
        if (msg.type === 'event' && msg.name === 'hello') {
            output.appendLine(`[xinsp2] backend v${msg.data?.version}`);
        } else if (msg.type === 'rsp') {
            // Responses are dispatched via pending map (simple approach).
            const handler = pendingRsp.get(msg.id);
            if (handler) {
                pendingRsp.delete(msg.id);
                handler(msg);
            }
        } else if (msg.type === 'vars') {
            viewerProvider.postVars(msg);
        } else if (msg.type === 'instances') {
            treeProvider.update(msg.instances ?? [], msg.params ?? []);
            setCtx('hasInstances', (msg.instances?.length ?? 0) > 0);
            lastInstanceCount = (msg.instances?.length ?? 0);
            setViewBadge(lastConnected, lastInstanceCount, lastPluginCount);
            // Recount plugin usage.
            const uses = new Map<string, number>();
            for (const i of (msg.instances ?? [])) {
                uses.set(i.plugin, (uses.get(i.plugin) || 0) + 1);
            }
            pluginTreeProvider.update(
                // Re-fetch plugin list to pick up any newly-appearing ones; but if
                // there's nothing fresh, keep the current set with updated counts.
                (pluginTreeProvider as any).plugins || [],
                uses,
            );
            // Fire-and-forget refetch in case a plugin got loaded.
            sendCmd('list_plugins').then((r: any) => {
                if (r?.ok && Array.isArray(r.data)) {
                    pluginTreeProvider.update(r.data as PluginInfo[], uses);
                    setCtx('hasPlugins', r.data.length > 0);
                }
            }).catch(() => {});
        } else if (msg.type === 'log') {
            output.appendLine(`[${msg.level}] ${msg.msg}`);
        }
    });

    client.on('binary', (buf: Buffer) => {
        if (buf.length < PREVIEW_HEADER_SIZE) return;
        const gid      = buf.readUInt32BE(0);
        const _codec   = buf.readUInt32BE(4);
        const width    = buf.readUInt32BE(8);
        const height   = buf.readUInt32BE(12);
        const _ch      = buf.readUInt32BE(16);
        const jpegBuf  = buf.subarray(PREVIEW_HEADER_SIZE);
        const b64      = jpegBuf.toString('base64');

        if ((gid >= 8000 && gid < 9000) || (gid >= 9000 && gid < 10000)) {
            // Config preview — route to the specific panel
            const panel = previewGidToPanel.get(gid);
            if (panel) {
                panel.webview.postMessage({
                    type: 'preview',
                    width, height,
                    jpeg: b64,
                });
            }
        } else {
            viewerProvider.postPreview(gid, width, height, b64);
        }
    });

    // Pending response map
    const pendingRsp = new Map<number, (msg: any) => void>();

    function sendCmd(name: string, args?: Record<string, unknown>): Promise<any> {
        return new Promise((resolve, reject) => {
            const id = nextId();
            pendingRsp.set(id, resolve);
            client!.sendCmd(id, name, args);
            setTimeout(() => {
                if (pendingRsp.has(id)) {
                    pendingRsp.delete(id);
                    reject(new Error(`cmd ${name} timed out`));
                }
            }, 120_000);
        });
    }

    // --- Commands ---

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.run', async () => {
            if (!client?.connected) {
                vscode.window.showWarningMessage('xInsp2: not connected to backend');
                return;
            }
            try {
                const rsp = await sendCmd('run');
                if (rsp.ok) {
                    vscode.window.setStatusBarMessage(
                        `xInsp2: run #${rsp.data?.run_id} (${rsp.data?.ms}ms)`, 5000);
                } else {
                    vscode.window.showErrorMessage(`xInsp2 run failed: ${rsp.error}`);
                }
            } catch (e: any) {
                vscode.window.showErrorMessage(`xInsp2: ${e.message}`);
            }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.compile', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showWarningMessage('xInsp2: open a .cpp file first');
                return;
            }
            const filePath = editor.document.uri.fsPath;
            if (!filePath.endsWith('.cpp')) {
                vscode.window.showWarningMessage('xInsp2: active file is not a .cpp');
                return;
            }
            await editor.document.save();
            output.appendLine(`[xinsp2] compiling ${filePath}...`);
            try {
                const rsp = await sendCmd('compile_and_load', { path: filePath });
                if (rsp.ok) {
                    output.appendLine('[xinsp2] compile ok: ' + (rsp.data?.dll ?? ''));
                    vscode.window.setStatusBarMessage('xInsp2: compiled', 3000);
                    // Refresh instance tree
                    sendCmd('list_instances').then((r: any) => {/* instances msg arrives via json handler */});
                } else {
                    output.appendLine('[xinsp2] compile FAILED: ' + (rsp.error ?? ''));
                    vscode.window.showErrorMessage(`xInsp2 compile failed: ${rsp.error}`);
                }
            } catch (e: any) {
                vscode.window.showErrorMessage(`xInsp2 compile: ${e.message}`);
            }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.saveProject', async () => {
            const uri = await vscode.window.showSaveDialog({
                filters: { 'xInsp2 Project': ['json'] },
                defaultUri: vscode.Uri.file('project.json'),
            });
            if (!uri) return;
            const rsp = await sendCmd('save_project', { path: uri.fsPath });
            if (rsp.ok) {
                vscode.window.showInformationMessage('xInsp2: project saved');
            } else {
                vscode.window.showErrorMessage('xInsp2: save failed — ' + rsp.error);
            }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.start', async () => {
            if (!client?.connected) { vscode.window.showWarningMessage('xInsp2: not connected'); return; }
            const rsp = await sendCmd('start');
            if (rsp.ok) { setCtx('running', true); vscode.window.setStatusBarMessage('xInsp2: continuous mode started', 3000); }
            else vscode.window.showErrorMessage('xInsp2 start failed: ' + rsp.error);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.stop', async () => {
            if (!client?.connected) return;
            const rsp = await sendCmd('stop');
            if (rsp.ok) { setCtx('running', false); vscode.window.setStatusBarMessage('xInsp2: stopped', 3000); }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.loadProject', async () => {
            const uris = await vscode.window.showOpenDialog({
                filters: { 'xInsp2 Project': ['json'] },
                canSelectMany: false,
            });
            if (!uris?.length) return;
            const rsp = await sendCmd('load_project', { path: uris[0].fsPath });
            if (rsp.ok) {
                vscode.window.showInformationMessage('xInsp2: project loaded');
                sendCmd('list_instances');
            } else {
                vscode.window.showErrorMessage('xInsp2: load failed — ' + rsp.error);
            }
        })
    );

    // --- Project & Plugin commands ---

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.createProject', async (folder?: string, name?: string) => {
            if (!client?.connected) return;
            if (!folder) {
                const uri = await vscode.window.showOpenDialog({ canSelectFolders: true, canSelectFiles: false, canSelectMany: false, openLabel: 'Create Project Here' });
                if (!uri?.length) return;
                folder = uri[0].fsPath;
            }
            if (!name) name = path.basename(folder);
            const rsp = await sendCmd('create_project', { folder, name });
            if (rsp.ok) {
                output.appendLine('[xinsp2] project created: ' + folder);
                vscode.window.setStatusBarMessage('xInsp2: project created', 3000);
                setCtx('hasProject', true);
                setCtx('hasInstances', false);
                addRecent(folder, name);
                currentProjectName = name;
                currentProjectPath = folder;
                lastProjectFolder = folder;          // for auto-respawn replay
                recomputeAutoRespawn();
                updateProjectStatus();
                treeProvider.setProjectOpen(true);
                openScriptIfExists(folder);
            }
            return rsp;
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openProject', async (folder?: string) => {
            if (!client?.connected) return;
            if (!folder) {
                const uri = await vscode.window.showOpenDialog({ canSelectFolders: true, canSelectFiles: false, canSelectMany: false, openLabel: 'Open Project' });
                if (!uri?.length) return;
                folder = uri[0].fsPath;
            }
            const rsp = await sendCmd('open_project', { folder });
            if (rsp.ok) {
                output.appendLine('[xinsp2] project opened: ' + folder);
                setCtx('hasProject', true);
                const n = rsp.data?.name || path.basename(folder);
                addRecent(folder, n);
                currentProjectName = n;
                currentProjectPath = folder;
                lastProjectFolder = folder;          // for auto-respawn replay
                recomputeAutoRespawn();
                updateProjectStatus();
                setCtx('hasInstances', (rsp.data?.instances?.length ?? 0) > 0);
                sendCmd('list_instances');
                treeProvider.setProjectOpen(true);
                openScriptIfExists(folder);
            }
            return rsp;
        })
    );

    // Open the project's inspection.cpp in an editor. Used by the auto-open
    // hooks above and the explicit xinsp2.openScript command.
    function openScriptIfExists(folder: string) {
        const fs = require('fs');
        const candidate = path.join(folder, 'inspection.cpp');
        if (fs.existsSync(candidate)) {
            vscode.workspace.openTextDocument(candidate).then(doc => {
                vscode.window.showTextDocument(doc, { preserveFocus: true, preview: false });
            }, () => { /* swallow */ });
        }
    }

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openScript', async () => {
            if (!currentProjectPath) {
                vscode.window.showWarningMessage('xInsp2: no project open');
                return;
            }
            openScriptIfExists(currentProjectPath);
        })
    );

    // Trigger policy picker — controls how the bus correlates frames from
    // multiple sources for this project.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.setTriggerPolicy', async () => {
            if (!currentProjectPath) {
                vscode.window.showWarningMessage('xInsp2: no project open');
                return;
            }
            type Pol = vscode.QuickPickItem & { id: 'any'|'all_required'|'leader_followers' };
            const choice = await vscode.window.showQuickPick<Pol>([
                { id: 'any',              label: 'Any',              description: 'Fire on every emit (default; back-compat)' },
                { id: 'all_required',     label: 'All required',     description: 'Wait until every listed source has emitted under the same trigger ID' },
                { id: 'leader_followers', label: 'Leader / followers', description: 'Fire on leader emit; attach latest follower frames' },
            ], { placeHolder: 'Trigger correlation policy' });
            if (!choice) return;

            let required: string[] = [];
            let leader = '';
            let window_ms = 100;

            if (choice.id === 'all_required' || choice.id === 'leader_followers') {
                // Get the list of available source instances.
                const inst = await sendCmd('list_instances');
                const sources: string[] = (inst?.data?.instances || []).map((i: any) => i.name);
                if (sources.length === 0) {
                    vscode.window.showWarningMessage('xInsp2: no instances yet — add some first');
                    return;
                }
                if (choice.id === 'all_required') {
                    const picks = await vscode.window.showQuickPick(sources,
                        { placeHolder: 'Select required source instances', canPickMany: true });
                    if (!picks?.length) return;
                    required = picks;
                } else {
                    const pick = await vscode.window.showQuickPick(sources,
                        { placeHolder: 'Select the leader source' });
                    if (!pick) return;
                    leader = pick;
                }
                const winStr = await vscode.window.showInputBox({
                    prompt: 'Correlation window (ms)',
                    value: '100',
                    validateInput: v => /^\d+$/.test(v) ? undefined : 'must be a positive integer',
                });
                if (!winStr) return;
                window_ms = parseInt(winStr, 10);
            }

            const r = await sendCmd('set_trigger_policy', {
                policy: choice.id, required, leader, window_ms,
            });
            if (r?.ok) {
                vscode.window.setStatusBarMessage(
                    `xInsp2: trigger policy → ${choice.label}`, 3000);
            } else {
                vscode.window.showErrorMessage(`xInsp2: ${r?.error || 'failed'}`);
            }
        })
    );

    // Close-project: asks backend to forget the project, resets state.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.closeProject', async () => {
            if (!client?.connected) return;
            const rsp = await sendCmd('close_project');
            if (rsp?.ok) {
                setCtx('hasProject', false);
                setCtx('hasInstances', false);
                currentProjectName = undefined;
                currentProjectPath = undefined;
                lastProjectFolder = null;            // user closed; don't replay on respawn
                updateProjectStatus();
                treeProvider.update([], []);
                treeProvider.setProjectOpen(false);
                vscode.window.setStatusBarMessage('xInsp2: project closed', 2000);
            }
        })
    );

    // Cert drill-down: read cert.json + run recertify, show as a webview.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.showPluginCert', async (treeItem?: any) => {
            // TreeItem label has the name + optional "  ×N" suffix; strip that.
            let pluginName = '';
            if (typeof treeItem === 'string') pluginName = treeItem;
            else if (treeItem?.label) pluginName = String(treeItem.label).split('  ×')[0];
            if (!pluginName) {
                const pick = await vscode.window.showInputBox({ prompt: 'Plugin name' });
                if (!pick) return;
                pluginName = pick;
            }
            const panel = vscode.window.createWebviewPanel(
                'xinsp2.pluginCert',
                `Cert · ${pluginName}`,
                vscode.ViewColumn.Active,
                { enableScripts: true, retainContextWhenHidden: true }
            );
            const render = async () => {
                // Find the plugin folder via list_plugins
                const all = await sendCmd('list_plugins');
                const p = (all?.data || []).find((x: any) => x.name === pluginName);
                const folder: string = p?.folder || '';
                let certJson: any = null;
                try {
                    const certPath = path.join(folder, 'cert.json');
                    if (folder && require('fs').existsSync(certPath)) {
                        certJson = JSON.parse(require('fs').readFileSync(certPath, 'utf8'));
                    }
                } catch {}
                panel.webview.html = certPanelHtml(pluginName, folder, p, certJson);
            };
            panel.webview.onDidReceiveMessage(async (msg: any) => {
                if (msg?.type === 'recert') {
                    const r = await sendCmd('recertify_plugin', { name: pluginName });
                    panel.webview.postMessage({ type: 'recert_result', ...r.data });
                    await render();
                    refreshPlugins();
                }
            });
            await render();
        })
    );

    // Remove an instance — right-click on tree item → "Remove Instance".
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.removeInstance', async (treeItem?: any) => {
            let instanceName = '';
            if (typeof treeItem === 'string') instanceName = treeItem;
            else if (treeItem?.label) instanceName = String(treeItem.label);
            if (!instanceName) return;
            const pick = await vscode.window.showWarningMessage(
                `Remove instance "${instanceName}"?`,
                { modal: true, detail: 'The instance and its on-disk folder will be deleted. This cannot be undone.' },
                'Remove (and delete folder)',
                'Remove (keep folder)',
            );
            if (!pick) return;
            const delete_folder = pick === 'Remove (and delete folder)';
            const r = await sendCmd('remove_instance', { name: instanceName, delete_folder });
            if (r?.ok) {
                sendCmd('list_instances');
                vscode.window.setStatusBarMessage(`xInsp2: removed "${instanceName}"`, 2500);
            } else {
                vscode.window.showErrorMessage(`Remove failed: ${r?.error || 'unknown'}`);
            }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.renameInstance', async (treeItem?: any) => {
            let oldName = '';
            if (typeof treeItem === 'string') oldName = treeItem;
            else if (treeItem?.label) oldName = String(treeItem.label);
            if (!oldName) return;
            const newName = await vscode.window.showInputBox({
                prompt: `Rename instance "${oldName}" to:`,
                value: oldName,
                validateInput: (v) =>
                    !v.trim()                          ? 'Name cannot be empty'
                    : !/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(v) ? 'Must start with a letter/underscore; identifier characters only'
                    : undefined,
            });
            if (!newName || newName === oldName) return;
            const r = await sendCmd('rename_instance', { name: oldName, new_name: newName });
            if (r?.ok) {
                sendCmd('list_instances');
                vscode.window.setStatusBarMessage(`xInsp2: renamed to "${newName}"`, 2500);
            } else {
                vscode.window.showErrorMessage(`Rename failed: ${r?.error || 'unknown'}`);
            }
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.recertifyPlugin', async (treeItem?: any) => {
            let pluginName = '';
            if (typeof treeItem === 'string') pluginName = treeItem;
            else if (treeItem?.label) pluginName = String(treeItem.label).split('  ×')[0];
            if (!pluginName) return;
            const r = await sendCmd('recertify_plugin', { name: pluginName });
            if (r?.ok) {
                const msg = r.data?.passed ? `✓ ${pluginName} re-certified` :
                            `⚠ ${pluginName}: ${r.data?.fail_count || 0} test(s) failed`;
                vscode.window.showInformationMessage(msg);
                refreshPlugins();
            } else {
                vscode.window.showErrorMessage(`re-cert failed: ${r?.error || 'unknown'}`);
            }
        })
    );

    // Recent projects via QuickPick
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openRecent', async () => {
            // Filter out paths that no longer exist on disk + persist the cleanup.
            const fs = require('fs');
            const all = getRecent();
            const live = all.filter(r => {
                try { return fs.existsSync(path.join(r.path, 'project.json')); }
                catch { return false; }
            });
            if (live.length !== all.length) {
                context.globalState.update(RECENT_KEY, live);
            }
            if (live.length === 0) {
                vscode.window.showInformationMessage(
                    all.length === 0
                        ? 'xInsp2: no recent projects yet'
                        : 'xInsp2: every recent project folder is gone — list cleared');
                return;
            }
            type Item = vscode.QuickPickItem & { path: string };
            const items: Item[] = live.map(r => ({
                label: r.name,
                description: r.path,
                detail: new Date(r.timestamp).toLocaleString(),
                path: r.path,
            }));
            const pick = await vscode.window.showQuickPick(items, { placeHolder: 'Open recent xInsp2 project' });
            if (!pick) return;
            await vscode.commands.executeCommand('xinsp2.openProject', pick.path);
        })
    );

    // First-run sample project: create a throwaway demo project with a
    // preconfigured mock_camera → blob_analysis pipeline so the user can
    // see something working immediately.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.createSampleProject', async () => {
            if (!client?.connected) {
                vscode.window.showWarningMessage('xInsp2: not connected to backend');
                return;
            }
            const { tmpdir } = require('os');
            const fs = require('fs');
            const sampleDir = path.join(tmpdir(),
                `xinsp2_sample_${new Date().toISOString().slice(0,10)}_${Date.now().toString(36)}`);
            const create = await sendCmd('create_project', {
                folder: sampleDir, name: 'xinsp2_sample',
            });
            if (!create?.ok) {
                vscode.window.showErrorMessage(`Sample project failed: ${create?.error || 'unknown'}`);
                return;
            }
            // Preconfigure a small pipeline — pick whichever plugins we have.
            const plugins: any[] = (await sendCmd('list_plugins'))?.data || [];
            const want = ['mock_camera', 'blob_analysis'];
            for (const p of want) {
                if (plugins.some(x => x.name === p)) {
                    await vscode.commands.executeCommand('xinsp2.createInstance',
                        p + '0', p);
                }
            }
            // Seed a working inspection.cpp that uses whichever instances we created.
            const scriptPath = path.join(sampleDir, 'inspection.cpp');
            const hasCam = plugins.some(x => x.name === 'mock_camera');
            const hasDet = plugins.some(x => x.name === 'blob_analysis');
            if (hasCam && hasDet) {
                fs.writeFileSync(scriptPath, `//
// xInsp2 sample — mock_camera → blob_analysis pipeline.
// Edit, save (compiles automatically), and click Run Inspection.
//
#include <xi/xi.hpp>
#include <xi/xi_ops.hpp>
#include <xi/xi_use.hpp>

using namespace xi::ops;

XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    auto& cam = xi::use("mock_camera0");
    auto& det = xi::use("blob_analysis0");

    auto img = cam.grab(500);
    if (img.empty()) {
        img = xi::Image(320, 240, 1);
        std::memset(img.data(), 0, 320*240);
        for (int y = 60; y < 100; ++y)
            for (int x = 60; x < 100; ++x) img.data()[y*320+x] = 255;
    }
    VAR(input, img);

    auto gray = toGray(img);
    VAR(gray, gray);

    auto out = det.process(xi::Record().image("gray", gray));
    VAR(detection, out);
    VAR(blob_count, out["blob_count"].as_int());
    VAR(binary,     out.get_image("binary"));
}
`);
            }
            // Open the script so the user sees something concrete.
            try {
                const doc = await vscode.workspace.openTextDocument(scriptPath);
                await vscode.window.showTextDocument(doc, vscode.ViewColumn.One);
            } catch {}
            vscode.window.showInformationMessage(
                `Sample project created at ${sampleDir}. Edit inspection.cpp or click Run.`);
        })
    );

    // Open the SDK getting-started doc from the welcome view
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openGettingStarted', async () => {
            const candidate = path.resolve(context.extensionPath, '..', 'sdk', 'GETTING_STARTED.md');
            try {
                const doc = await vscode.workspace.openTextDocument(candidate);
                await vscode.window.showTextDocument(doc);
            } catch {
                vscode.window.showInformationMessage(
                    `xInsp2: GETTING_STARTED.md not found at ${candidate}. Set xinsp2.sdkPath if installed elsewhere.`);
            }
        })
    );

    // Manually restart the backend (for when it hangs / crashes, or to
    // resume after the auto-respawn rate-limit has tripped).
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.restartBackend', async () => {
            // Reset the rate-limit so the user gets a fresh budget.
            recentRespawnsMs.length = 0;
            // Suppress the imminent-exit handler so it doesn't double-spawn.
            intendedRunning = false;
            if (backend) { try { backend.kill(); } catch {} backend = null; }
            setCtx('connected', false);
            setCtx('hasProject', false);
            setCtx('running', false);
            output.appendLine(`[xinsp2] manual restart`);
            intendedRunning = true;
            // Reuse the same spawn-and-watch helper so subsequent crashes
            // are handled by auto-respawn just like a fresh activation.
            if (spawnAndWatchHandle) {
                spawnAndWatchHandle();
            } else {
                // Fallback (remote mode / no autoStart) — just connect.
                output.appendLine('[xinsp2] no spawn handle (remote mode?); just reconnecting');
            }
            setTimeout(() => client!.connect(), 500);
        })
    );

    // ---- Plugin-manager tree actions ---------------------------------
    async function refreshPlugins() {
        const r = await sendCmd('list_plugins');
        if (r?.ok && Array.isArray(r.data)) {
            pluginTreeProvider.update(r.data as PluginInfo[]);
            setCtx('hasPlugins', r.data.length > 0);
            lastPluginCount = r.data.length;
            setViewBadge(lastConnected, lastInstanceCount, lastPluginCount);
        }
    }

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.refreshPlugins', async () => {
            if (!client?.connected) return;
            // Rescan every known folder, then refresh the tree.
            for (const dir of config.get<string[]>('extraPluginDirs', [])) {
                await sendCmd('rescan_plugins', { dir });
            }
            await sendCmd('rescan_plugins'); // built-in dir
            await refreshPlugins();
            vscode.window.setStatusBarMessage('xInsp2: plugins rescanned', 2000);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.addPluginFolder', async () => {
            const uris = await vscode.window.showOpenDialog({
                canSelectFolders: true, canSelectFiles: false, canSelectMany: false,
                openLabel: 'Add plugin folder',
            });
            if (!uris?.length) return;
            const folder = uris[0].fsPath;
            const current = config.get<string[]>('extraPluginDirs', []);
            if (current.some(d => path.resolve(d).toLowerCase() === path.resolve(folder).toLowerCase())) {
                vscode.window.showInformationMessage(`xInsp2: "${folder}" already in plugin dirs`);
                return;
            }
            const next = [...current, folder];
            await config.update('extraPluginDirs', next, vscode.ConfigurationTarget.Global);
            pluginTreeProvider.setRemovableFolders(next);
            if (client?.connected) await sendCmd('rescan_plugins', { dir: folder });
            await refreshPlugins();
            vscode.window.showInformationMessage(`xInsp2: added ${folder}`);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.removePluginFolder', async (treeItem?: any) => {
            // Invoked from the tree's inline action: arg is the TreeItem.
            const folder = treeItem?.resourceUri?.fsPath || treeItem?.tooltip;
            if (!folder || typeof folder !== 'string') {
                vscode.window.showWarningMessage('xInsp2: no folder selected');
                return;
            }
            const current = config.get<string[]>('extraPluginDirs', []);
            const next = current.filter(d => path.resolve(d).toLowerCase() !== path.resolve(folder).toLowerCase());
            await config.update('extraPluginDirs', next, vscode.ConfigurationTarget.Global);
            pluginTreeProvider.setRemovableFolders(next);
            // Note: host keeps the plugin registered until restart. Tell the user.
            vscode.window.showWarningMessage(
                `xInsp2: "${folder}" removed from settings. Restart the backend to fully unload.`,
                'Restart Backend'
            ).then(choice => {
                if (choice === 'Restart Backend') vscode.commands.executeCommand('xinsp2.restartBackend');
            });
            await refreshPlugins();
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.revealPluginFolder', async (treeItem?: any) => {
            const folder = treeItem?.resourceUri?.fsPath || treeItem?.tooltip;
            if (!folder) return;
            try { await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(folder)); } catch {}
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.listPlugins', async () => {
            if (!client?.connected) return;
            return sendCmd('list_plugins');
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.createInstance', async (instanceName?: string, pluginName?: string) => {
            if (!client?.connected) return;
            if (!pluginName) {
                const plugins = await sendCmd('list_plugins');
                if (!plugins.ok) return;
                const pick = await vscode.window.showQuickPick(
                    plugins.data.map((p: any) => ({ label: p.name, description: p.description })),
                    { placeHolder: 'Select plugin type' }
                );
                if (!pick) return;
                pluginName = pick.label;
            }
            if (!instanceName) {
                instanceName = await vscode.window.showInputBox({ prompt: 'Instance name', value: pluginName + '0' });
                if (!instanceName) return;
            }
            const rsp = await sendCmd('create_instance', { name: instanceName, plugin: pluginName });
            if (rsp.ok) {
                output.appendLine(`[xinsp2] instance created: ${instanceName} (${pluginName})`);
                sendCmd('list_instances');
            }
            return rsp;
        })
    );

    // Open a plugin's web UI in a webview panel for a specific instance
    const pluginUIPanels = new Map<string, vscode.WebviewPanel>();
    const previewGidToPanel = new Map<number, vscode.WebviewPanel>();

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openInstanceUI', async (arg1?: any, arg2?: string) => {
            if (!client?.connected) return;
            // Inline view/item button passes the TreeItem as arg1; programmatic
            // callers pass (instanceName, pluginName).
            let instanceName: string | undefined;
            let pluginName: string | undefined;
            if (typeof arg1 === 'string') {
                instanceName = arg1;
                pluginName = arg2;
            } else if (arg1 && typeof arg1 === 'object') {
                instanceName = arg1.label ?? arg1.name;
                pluginName = arg1.description ?? arg1.plugin;
            }
            if (!instanceName || !pluginName) return;

            // Check if panel already open
            const existing = pluginUIPanels.get(instanceName);
            if (existing) { existing.reveal(); return; }

            // Get the plugin UI path from the backend
            const uiRsp = await sendCmd('get_plugin_ui', { plugin: pluginName });
            if (!uiRsp.ok) {
                vscode.window.showWarningMessage(`No UI for plugin ${pluginName}`);
                return;
            }
            const uiPath = uiRsp.data.ui_path;

            // Read the UI HTML
            const fs = require('fs');
            const htmlPath = path.join(uiPath, 'index.html');
            if (!fs.existsSync(htmlPath)) {
                vscode.window.showWarningMessage(`UI not found: ${htmlPath}`);
                return;
            }
            let html = fs.readFileSync(htmlPath, 'utf8');

            // Inject a test shim that lets E2E tests drive DOM events as if a
            // real user clicked / typed. Listens for postMessages from the
            // extension and dispatches the corresponding browser events.
            const testShim = `
<script>
(function(){
  window.addEventListener('message', function(e){
    var m = e.data;
    if (!m || typeof m.type !== 'string') return;
    if (m.type === '__xi_click') {
      var el = document.querySelector(m.selector);
      if (el) el.click();
    } else if (m.type === '__xi_set_input') {
      var el = document.querySelector(m.selector);
      if (el) {
        el.value = m.value;
        el.dispatchEvent(new Event('input', {bubbles:true}));
        el.dispatchEvent(new Event('change', {bubbles:true}));
      }
    }
  });
})();
</script>`;
            html = html.replace('</body>', testShim + '</body>');

            // Create webview panel. localResourceRoots gates which files the
            // webview can load — include the extension dir (so @vscode-elements
            // can be served) and the plugin's own folder (for images/assets
            // it might reference from the UI).
            const veLibDir = vscode.Uri.joinPath(context.extensionUri,
                'node_modules', '@vscode-elements', 'elements', 'dist');
            const panel = vscode.window.createWebviewPanel(
                'xinsp2.pluginUI',
                `${instanceName} (${pluginName})`,
                vscode.ViewColumn.Two,
                {
                    enableScripts: true,
                    localResourceRoots: [
                        veLibDir,
                        vscode.Uri.file(uiPath),
                    ],
                }
            );

            // Inject @vscode-elements (Microsoft's web-components for VS Code
            // webviews — native-looking buttons, inputs, tabs, badges that
            // inherit the user's theme). If the plugin doesn't use any of the
            // <vscode-*> tags this is just ~230KB of dead code in the webview.
            const veUri = panel.webview.asWebviewUri(
                vscode.Uri.joinPath(veLibDir, 'bundled.js')
            );
            const veScriptTag = `<script type="module" src="${veUri}"></script>`;
            // Put it in the <head> if we can find one, else prepend to body.
            if (html.includes('</head>')) {
                html = html.replace('</head>', `  ${veScriptTag}\n</head>`);
            } else {
                html = html.replace('<body>', `<body>\n${veScriptTag}`);
            }
            panel.webview.html = html;
            pluginUIPanels.set(instanceName, panel);
            panel.onDidDispose(() => {
                pluginUIPanels.delete(instanceName);
                for (const [gid, p] of previewGidToPanel) {
                    if (p === panel) previewGidToPanel.delete(gid);
                }
            });

            // Wire postMessage ↔ exchange_instance + preview polling
            panel.webview.onDidReceiveMessage(async (msg: any) => {
                if (msg.type === 'exchange' && msg.cmd) {
                    const rsp = await sendCmd('exchange_instance', {
                        name: instanceName,
                        cmd: msg.cmd,
                    });
                    if (rsp.ok && rsp.data) {
                        panel.webview.postMessage({ type: 'status', ...JSON.parse(
                            typeof rsp.data === 'string' ? rsp.data : JSON.stringify(rsp.data)
                        )});
                    }
                } else if (msg.type === 'request_preview') {
                    sendCmd('preview_instance', { name: instanceName }).then((rsp: any) => {
                        if (rsp.ok && rsp.data?.gid) {
                            previewGidToPanel.set(rsp.data.gid, panel);
                        }
                    }).catch(() => {});
                } else if (msg.type === 'request_process') {
                    // Plugin UI wants to run process() and see results
                    sendCmd('exchange_instance', {
                        name: instanceName,
                        cmd: msg.cmd || { command: 'get_status' },
                    }).then((rsp: any) => {
                        if (rsp.ok && rsp.data) {
                            const parsed = typeof rsp.data === 'string' ? JSON.parse(rsp.data) : rsp.data;
                            panel.webview.postMessage({ type: 'process_result', ...parsed });
                        }
                    }).catch(() => {});
                }
            });

            // Request initial status
            const statusRsp = await sendCmd('exchange_instance', {
                name: instanceName,
                cmd: { command: 'get_status' },
            });
            if (statusRsp.ok && statusRsp.data) {
                const parsed = typeof statusRsp.data === 'string' ? JSON.parse(statusRsp.data) : statusRsp.data;
                panel.webview.postMessage({ type: 'status', ...parsed });
            }
        })
    );

    // Expose a test API for E2E tests
    const testAPI = {
        sendCmd,
        get connected() { return client?.connected ?? false; },
        waitConnected: async (timeoutMs = 10000) => {
            const t0 = Date.now();
            while (!client?.connected && Date.now() - t0 < timeoutMs) {
                await new Promise(r => setTimeout(r, 100));
            }
            return client?.connected ?? false;
        },
        registerPluginPanel: (name: string, panel: vscode.WebviewPanel) => {
            pluginUIPanels.set(name, panel);
            panel.onDidDispose(() => pluginUIPanels.delete(name));
        },
        registerPreviewGid: (gid: number, panel: vscode.WebviewPanel) => {
            previewGidToPanel.set(gid, panel);
        },
        // Simulates a user clicking a button inside the plugin's webview UI:
        // runs the same path as panel.webview.onDidReceiveMessage, including
        // the status post-back so the webview repaints (e.g., "Streaming").
        simulateWebviewExchange: async (instanceName: string, cmd: any) => {
            const rsp = await sendCmd('exchange_instance', { name: instanceName, cmd });
            const panel = pluginUIPanels.get(instanceName);
            if (panel && rsp.ok && rsp.data) {
                const parsed = typeof rsp.data === 'string' ? JSON.parse(rsp.data) : rsp.data;
                panel.webview.postMessage({ type: 'status', ...parsed });
            }
            return rsp;
        },
        // Drive the DOM inside a plugin's webview as if a real user did it.
        // Posts a control message that the injected test shim listens for.
        clickInWebview: (instanceName: string, selector: string) => {
            const panel = pluginUIPanels.get(instanceName);
            if (!panel) return false;
            return panel.webview.postMessage({ type: '__xi_click', selector });
        },
        setInputInWebview: (instanceName: string, selector: string, value: string | number) => {
            const panel = pluginUIPanels.get(instanceName);
            if (!panel) return false;
            return panel.webview.postMessage({ type: '__xi_set_input', selector, value: String(value) });
        },
    };

    // Live-session runner: load <plugin>/tests/test_ui.cjs and feed it the
    // same `h` helpers the CLI launcher provides. Avoids the 10–20s VS Code
    // cold-start when iterating on a plugin's UI test.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.runPluginUITests', async (folderHint?: string) => {
            let pluginFolder = folderHint;
            if (!pluginFolder) {
                const uris = await vscode.window.showOpenDialog({
                    canSelectFolders: true, canSelectFiles: false, canSelectMany: false,
                    openLabel: 'Pick plugin folder',
                });
                if (!uris?.length) return;
                pluginFolder = uris[0].fsPath;
            }
            const fs = require('fs') as typeof import('fs');
            const testFile = path.join(pluginFolder, 'tests', 'test_ui.cjs');
            if (!fs.existsSync(testFile)) {
                vscode.window.showErrorMessage(`No tests/test_ui.cjs in ${pluginFolder}`);
                return;
            }
            // Helpers live in the SDK. In the dev workspace we resolve them
            // relative to this extension; configurable via xinsp2.sdkPath.
            const sdkPath = vscode.workspace.getConfiguration('xinsp2').get<string>('sdkPath')
                || path.resolve(context.extensionPath, '..', 'sdk');
            const helpersPath = path.join(sdkPath, 'testing', 'helpers.cjs');
            if (!fs.existsSync(helpersPath)) {
                vscode.window.showErrorMessage(`SDK helpers not found at ${helpersPath}. Set xinsp2.sdkPath.`);
                return;
            }

            // Make sure the plugin's parent folder is scanned, so this
            // plugin is loadable in the live host without a restart.
            try {
                await sendCmd('rescan_plugins', { dir: path.dirname(pluginFolder) });
            } catch { /* command may not exist; harmless */ }

            output.appendLine(`[xinsp2] running UI test for ${pluginFolder}`);
            output.show(true);
            const { makeHelpers } = require(helpersPath);
            const h = await makeHelpers(pluginFolder);

            // Reload the test module each time so the user can edit + re-run
            delete require.cache[require.resolve(testFile)];
            const mod = require(testFile);
            const runFn = (typeof mod === 'function') ? mod
                        : (mod && typeof mod.run === 'function') ? mod.run : null;
            if (!runFn) {
                vscode.window.showErrorMessage(`${testFile} must export run(h)`);
                return;
            }

            try {
                await runFn(h);
                if (h.failures.length === 0) {
                    vscode.window.showInformationMessage(
                        `Plugin UI test passed (${h.passes.length} assertions)`);
                } else {
                    vscode.window.showErrorMessage(
                        `Plugin UI test: ${h.failures.length} failure(s) — see Output`);
                    for (const f of h.failures) output.appendLine(`  ✗ ${f}`);
                }
            } catch (e: any) {
                vscode.window.showErrorMessage(`Plugin UI test threw: ${e.message}`);
                output.appendLine(`[xinsp2] threw: ${e.stack || e}`);
            }
        })
    );

    // --- Auto-compile on save (S2) ---
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(async (doc) => {
            if (!doc.fileName.endsWith('.cpp')) return;
            if (!client?.connected) return;
            output.appendLine(`[xinsp2] auto-compile: ${doc.fileName}`);
            try {
                const rsp = await sendCmd('compile_and_load', { path: doc.fileName });
                if (rsp.ok) {
                    output.appendLine('[xinsp2] auto-compile ok');
                    vscode.window.setStatusBarMessage('xInsp2: recompiled', 2000);
                    sendCmd('list_instances');
                    // If in continuous mode, the next trigger will use the new code.
                    // For single-shot, auto-run after compile.
                    if (!g_continuous) {
                        await sendCmd('run');
                    }
                } else {
                    output.appendLine('[xinsp2] auto-compile FAILED: ' + (rsp.error ?? ''));
                    vscode.window.showErrorMessage('xInsp2: compile failed — check Output');
                }
            } catch (e: any) {
                output.appendLine('[xinsp2] auto-compile error: ' + e.message);
            }
        })
    );

    let g_continuous = false;
    client.on('json', (msg: any) => {
        if (msg.type === 'rsp' && msg.data?.started) g_continuous = true;
        if (msg.type === 'rsp' && msg.data?.stopped)  g_continuous = false;
    });

    // --- Start backend ---

    if (isRemote) {
        // Remote mode: never spawn locally; just connect. autoStart is
        // ignored (docs note this).
        output.appendLine(`[xinsp2] connecting to remote ${wsUrl}`);
        client!.connect();
    } else if (autoStart) {
        // Single function used by both initial spawn and auto-respawn.
        // Hoisted to the activate() closure so xinsp2.restartBackend
        // below can reuse it (gives identical respawn behaviour after
        // a manual restart).
        const _spawnAndWatch = () => {
            const exe = findBackendExe(context);
            const args = [`--port=${port}`];
            for (const dir of extraPluginDirs) args.push(`--plugins-dir=${dir}`);
            output.appendLine(`[xinsp2] starting ${exe} ${args.join(' ')}`);
            backend = spawn(exe, args, {
                stdio: ['ignore', 'pipe', 'pipe'],
            });
            backend.stdout?.on('data', (d: Buffer) => output.append(d.toString()));
            backend.stderr?.on('data', (d: Buffer) => output.append(d.toString()));
            backend.on('exit', (code: number | null, signal: string | null) => {
                output.appendLine(`[xinsp2] backend exited (code=${code} signal=${signal})`);
                backend = null;
                if (!intendedRunning) return;   // clean shutdown
                // Per-project / workspace opt-out — recompute in case
                // the user just edited project.json.
                recomputeAutoRespawn();
                if (!autoRespawnEnabled) {
                    output.appendLine(`[xinsp2] auto-respawn disabled (project.json or xinsp2.autoRespawn) — click Restart Backend to recover`);
                    vscode.window.showWarningMessage(
                        'xInsp2 backend exited. Auto-respawn is disabled for this project — click Restart Backend when ready.',
                        'Restart Backend'
                    ).then(c => { if (c) vscode.commands.executeCommand('xinsp2.restartBackend'); });
                    intendedRunning = false;
                    return;
                }
                // Rate-limit: prune to last 60 s, bail if too many.
                const now = Date.now();
                while (recentRespawnsMs.length > 0 && recentRespawnsMs[0] < now - 60_000) {
                    recentRespawnsMs.shift();
                }
                if (recentRespawnsMs.length >= MAX_RESPAWNS_PER_MINUTE) {
                    output.appendLine(`[xinsp2] backend crashed ${recentRespawnsMs.length}× in last minute — giving up. Use "Restart Backend" to try again.`);
                    vscode.window.showErrorMessage(
                        `xInsp2 backend crashed ${recentRespawnsMs.length}× in 60s. Auto-respawn paused — check Output panel and click Restart Backend when ready.`
                    );
                    intendedRunning = false;
                    return;
                }
                recentRespawnsMs.push(now);
                output.appendLine(`[xinsp2] auto-respawn in 1.5 s (${recentRespawnsMs.length}/${MAX_RESPAWNS_PER_MINUTE} this minute)`);
                vscode.window.setStatusBarMessage('xInsp2: backend crashed — respawning…', 4000);
                setTimeout(() => {
                    if (!intendedRunning) return;
                    _spawnAndWatch();
                    setTimeout(() => client?.connect(), 500);
                }, 1500);
            });
        };
        // Stash for restartBackend command (closure captures activate's locals)
        spawnAndWatchHandle = _spawnAndWatch;
        intendedRunning = true;
        _spawnAndWatch();
        // Give it a moment to bind, then connect.
        setTimeout(() => client!.connect(), 500);
    } else {
        client.connect();
    }

    // Cleanup
    context.subscriptions.push({
        dispose: () => {
            intendedRunning = false;     // suppress auto-respawn
            client?.dispose();
            if (backend) {
                try { backend.kill(); } catch {}
            }
        },
    });

    return { __test__: testAPI };
}

export function deactivate() {
    client?.dispose();
    if (backend) {
        try { backend.kill(); } catch {}
    }
}
