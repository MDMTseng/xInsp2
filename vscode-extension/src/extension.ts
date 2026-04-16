import * as vscode from 'vscode';
import * as path from 'path';
import { spawn, ChildProcess } from 'child_process';
import { WsClient } from './wsClient';
import { InstanceTreeProvider } from './instanceTree';
import { ViewerProvider } from './viewerProvider';
import { PREVIEW_HEADER_SIZE } from './protocol';

let backend: ChildProcess | null = null;
let client: WsClient | null = null;
let cmdId = 1;
const nextId = () => cmdId++;

function findBackendExe(context: vscode.ExtensionContext): string {
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
    const output = vscode.window.createOutputChannel('xInsp2');

    // Tree view
    const treeProvider = new InstanceTreeProvider();
    vscode.window.createTreeView('xinsp2.instances', { treeDataProvider: treeProvider });

    // Viewer webview (side panel)
    const viewerProvider = new ViewerProvider(context.extensionUri);
    context.subscriptions.push(
        vscode.window.registerWebviewViewProvider('xinsp2.viewer', viewerProvider, {
            webviewOptions: { retainContextWhenHidden: true },
        })
    );

    // WS client
    client = new WsClient({ url: `ws://127.0.0.1:${port}` });

    client.on('open', () => {
        output.appendLine('[xinsp2] connected to backend');
        vscode.window.setStatusBarMessage('xInsp2: connected', 3000);
    });

    client.on('close', () => {
        output.appendLine('[xinsp2] disconnected');
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
        viewerProvider.postPreview(gid, width, height, b64);
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
            if (rsp.ok) vscode.window.setStatusBarMessage('xInsp2: continuous mode started', 3000);
            else vscode.window.showErrorMessage('xInsp2 start failed: ' + rsp.error);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.stop', async () => {
            if (!client?.connected) return;
            const rsp = await sendCmd('stop');
            if (rsp.ok) vscode.window.setStatusBarMessage('xInsp2: stopped', 3000);
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
                sendCmd('list_instances');
            }
            return rsp;
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

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openInstanceUI', async (instanceName?: string, pluginName?: string) => {
            if (!client?.connected || !instanceName || !pluginName) return;

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

            // Create webview panel
            const panel = vscode.window.createWebviewPanel(
                'xinsp2.pluginUI',
                `${instanceName} (${pluginName})`,
                vscode.ViewColumn.Two,
                { enableScripts: true }
            );
            panel.webview.html = html;
            pluginUIPanels.set(instanceName, panel);
            panel.onDidDispose(() => pluginUIPanels.delete(instanceName));

            // Wire postMessage ↔ exchange_instance
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
    };

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

    if (autoStart) {
        const exe = findBackendExe(context);
        output.appendLine(`[xinsp2] starting ${exe} --port=${port}`);
        backend = spawn(exe, [`--port=${port}`], {
            stdio: ['ignore', 'pipe', 'pipe'],
        });
        backend.stdout?.on('data', (d: Buffer) => output.append(d.toString()));
        backend.stderr?.on('data', (d: Buffer) => output.append(d.toString()));
        backend.on('exit', (code) => {
            output.appendLine(`[xinsp2] backend exited (${code})`);
            backend = null;
        });
        // Give it a moment to bind, then connect.
        setTimeout(() => client!.connect(), 500);
    } else {
        client.connect();
    }

    // Cleanup
    context.subscriptions.push({
        dispose: () => {
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
