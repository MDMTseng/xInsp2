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
}

export function deactivate() {
    client?.dispose();
    if (backend) {
        try { backend.kill(); } catch {}
    }
}
