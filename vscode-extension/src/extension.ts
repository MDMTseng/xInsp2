import * as vscode from 'vscode';
import * as path from 'path';
import * as net from 'net';
import { spawn, ChildProcess } from 'child_process';
import { WsClient } from './wsClient';
import { InstanceTreeProvider } from './instanceTree';
import { ViewerProvider } from './viewerProvider';
import { InstanceCodeLensProvider } from './instanceCodeLens';
import { PluginRegistry, PluginInfo } from './pluginRegistry';
import { PREVIEW_HEADER_SIZE } from './protocol';
import { TEMPLATE_CHOICES, TemplateId, locateSdkRoot, renderPluginFiles }
    from './projectPluginTemplates';
import { renderProjectSettingsHtml } from './projectSettingsHtml';
import { renderPluginBrowserHtml, PBModel, PBRoot, PBTreeNode, PBPlugin } from './pluginBrowser';
import { ImageViewerPanel } from './imageViewerPanel';
import { resolveBackendMode } from './backendMode.mjs';

// --- Plugin Browser model building (pure; reads project.json + the filesystem) ---
function pbExpandRoot(raw: string, projectFolder: string): string {
    let r = raw.replace(/\$\{([^}]+)\}/g, (_m, v) => process.env[v] || '');
    if (r.startsWith('~')) r = (process.env.USERPROFILE || process.env.HOME || '~') + r.slice(1);
    if (!path.isAbsolute(r)) r = path.join(projectFolder, r);
    return path.normalize(r);
}
function pbHasSourceOrCmake(folder: string): boolean {
    const fs = require('fs') as typeof import('fs');
    if (fs.existsSync(path.join(folder, 'CMakeLists.txt'))) return true;
    for (const d of [folder, path.join(folder, 'src')]) {
        try { if (fs.readdirSync(d).some((f) => f.endsWith('.cpp'))) return true; } catch { /* ignore */ }
    }
    return false;
}
function pbScanTree(dir: string, relBase: string, depth: number): PBTreeNode[] {
    const fs = require('fs') as typeof import('fs');
    const out: PBTreeNode[] = [];
    let names: string[] = [];
    try { names = fs.readdirSync(dir).sort(); } catch { return out; }
    for (const name of names) {
        if (name === 'build' || name === 'node_modules' || name.startsWith('.')) continue;
        const full = path.join(dir, name);
        try { if (!fs.statSync(full).isDirectory()) continue; } catch { continue; }
        const rel = relBase ? relBase + '/' + name : name;
        if (fs.existsSync(path.join(full, 'plugin.json'))) {
            out.push({ name, rel, isPlugin: true, needsCompile: pbHasSourceOrCmake(full), children: [] });
        } else if (depth > 0) {
            const children = pbScanTree(full, rel, depth - 1);
            if (children.length) out.push({ name, rel, isPlugin: false, needsCompile: false, children });
        }
    }
    return out;
}
// Live plugin info (loaded / uses / origin …) folded into the static project.json
// model so the browser can show runtime status the old tree used to. Supplied by
// the extension (the pure model build degrades gracefully when it's absent).
interface PBLive {
    byName: Map<string, PluginInfo>;
    uses: (name: string) => number;
    isRemovable: (folder: string) => boolean;
}
function pbBuildModel(projectFolder: string, live?: PBLive): PBModel {
    const fs = require('fs') as typeof import('fs');
    let pj: any = {};
    try { pj = JSON.parse(fs.readFileSync(path.join(projectFolder, 'project.json'), 'utf8')); } catch { /* ignore */ }
    const obj = (pj.plugins && typeof pj.plugins === 'object' && !Array.isArray(pj.plugins)) ? pj.plugins : {};
    const added: PBPlugin[] = Object.keys(obj).map((k) => {
        const info = live?.byName.get(k);
        return {
            label: k, path: obj[k]?.path ?? k, compile: !!obj[k]?.compile,
            loaded: info?.loaded, uses: live?.uses(k),
            origin: info?.origin, prebuilt: info?.prebuilt,
            folder: info?.origin === 'project' ? (info?.source_dir || info?.folder) : info?.folder,
        };
    });
    let dirsRaw: string[] = Array.isArray(pj.plugin_dirs) ? pj.plugin_dirs.slice() : [];
    const usedFallback = !dirsRaw.length;
    if (usedFallback) dirsRaw = ['./plugins'];
    const roots: PBRoot[] = dirsRaw.map((raw) => {
        const resolved = pbExpandRoot(raw, projectFolder);
        const exists = fs.existsSync(resolved);
        // The materialized "(default)" fallback is never user-removable.
        const removable = !usedFallback && !!live?.isRemovable(resolved);
        return { raw: usedFallback ? raw + '  (default)' : raw, resolved, exists, removable,
                 tree: exists ? pbScanTree(resolved, '', 4) : [] };
    });
    return { projectName: pj.name || path.basename(projectFolder), added, addedPaths: added.map((p) => p.path), roots };
}

let backend: ChildProcess | null = null;
// Auto-respawn state. `intendedRunning` is true while the extension wants
// the backend up — set to false on `dispose()` and on the explicit
// shutdown command, so a clean exit doesn't trigger a respawn.
let intendedRunning = false;
// Sliding window of recent respawn timestamps (ms epoch) for rate limit.
const recentRespawnsMs: number[] = [];
const MAX_RESPAWNS_PER_MINUTE = 5;
// Default inspection-script filename when a project.json doesn't name one.
// The script name is normally read from project.json's `script` field; this is
// only the fallback. Single source of truth so the convention lives in one place.
const DEFAULT_SCRIPT_NAME = 'inspect.cpp';
// Last project we know was opened. Set by handlers below; replayed on
// every successful (re)connect so a respawned backend lands the user
// back on their working tree.
let lastProjectFolder: string | null = null;
// Set inside activate(); used by xinsp2.restartBackend so manual restarts
// reuse the auto-respawn-aware spawn helper.
let spawnAndWatchHandle: (() => void) | null = null;
// True when a supervisor outside the extension (xinsp-fe.exe on a line) owns
// the backend process. In attach mode the extension connects read/operator-only
// and never spawns or respawns — lifecycle + safe-state belong to the FE.
let attachMode = false;

// The port this window's backend actually ended up on (managed mode auto-assigns
// a free one). 0 until resolved. Shown in the status bar so multiple project
// windows are distinguishable.
let backendPortInUse = 0;

// Quick "is something already accepting on this local port?" probe. Used to
// resolve backendMode:"auto" — if a backend (FE-managed or otherwise) is
// already up, attach to it instead of spawning a competing one.
function isPortOpen(port: number, timeoutMs = 400): Promise<boolean> {
    return new Promise((resolve) => {
        const sock = new net.Socket();
        let done = false;
        const finish = (open: boolean) => {
            if (done) return;
            done = true;
            try { sock.destroy(); } catch { /* ignore */ }
            resolve(open);
        };
        sock.setTimeout(timeoutMs);
        sock.once('connect', () => finish(true));
        sock.once('timeout', () => finish(false));
        sock.once('error', () => finish(false));
        sock.connect(port, '127.0.0.1');
    });
}

// Find a free TCP port at or above `base` (probes base, base+1, …). Used in
// managed mode so each VS Code window spawns its own backend on its own port —
// multiple projects can run side by side without colliding on a fixed port.
async function findFreePort(base: number, span = 64): Promise<number> {
    for (let p = base; p < base + span; p++) {
        if (!(await isPortOpen(p, 250))) return p;
    }
    return base;   // fallback: let the spawn fail loudly rather than loop forever
}

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

// Render a plugin's I/O contract (from its plugin.json `manifest` block) as a
// hover. Everything substantive goes in fenced code blocks so the user can
// SELECT + COPY the names/types straight out of the popup — a view-only hover is
// useless when you're trying to type a key. Free-form/partial manifests degrade
// gracefully.
function renderPluginHover(instanceName: string, pluginName: string, manifest: any): vscode.MarkdownString {
    const md = new vscode.MarkdownString();
    md.appendMarkdown(`**${instanceName}** · plugin \`${pluginName}\`\n\n`);

    if (!manifest || typeof manifest !== 'object') {
        md.appendMarkdown(
            `_No \`manifest\` schema declared in this plugin's \`plugin.json\`._\n\n` +
            `Add an \`inputs\` / \`outputs\` / \`params\` block to surface its I/O contract here.`,
        );
        return md;
    }

    // Align columns into a monospace table for the code block.
    const fmt = (arr: any[], cols: (it: any) => string[]): string => {
        const rows = arr.map(cols);
        const w: number[] = [];
        for (const r of rows) r.forEach((c, i) => { w[i] = Math.max(w[i] || 0, c.length); });
        return rows.map(r => r.map((c, i) => c.padEnd(w[i])).join('  ').replace(/\s+$/, '')).join('\n');
    };
    const kindOf = (it: any) => String(it.kind ?? it.type ?? '');
    const trunc  = (s: any, n = 72) => { const t = String(s ?? ''); return t.length > n ? t.slice(0, n - 1) + '…' : t; };

    const inputs  = Array.isArray(manifest.inputs)  ? manifest.inputs  : [];
    const outputs = Array.isArray(manifest.outputs) ? manifest.outputs : [];
    const params  = Array.isArray(manifest.params)  ? manifest.params  : [];

    if (inputs.length) {
        md.appendMarkdown(`**Inputs**`);
        md.appendCodeblock(fmt(inputs, (it) => [
            String(it.name ?? '?'),
            kindOf(it) + ((it.optional === true || it.required === false) ? ' (optional)' : ''),
            trunc(it.doc),
        ]), 'text');
    }
    if (outputs.length) {
        md.appendMarkdown(`**Outputs**`);
        md.appendCodeblock(fmt(outputs, (it) => [
            String(it.name ?? '?'),
            kindOf(it),
            trunc(it.doc),
        ]), 'text');
    }
    if (params.length) {
        md.appendMarkdown(`**Params**`);
        md.appendCodeblock(fmt(params, (it) => {
            const range = Array.isArray(it.enum) ? `{${it.enum.join('|')}}`
                        : (it.min !== undefined || it.max !== undefined) ? `[${it.min ?? ''}..${it.max ?? ''}]`
                        : '';
            return [
                String(it.name ?? '?'),
                String(it.type ?? ''),
                it.default !== undefined ? `= ${JSON.stringify(it.default)}` : '',
                range,
            ];
        }), 'text');
    }
    if (!inputs.length && !outputs.length && !params.length) {
        md.appendMarkdown(`_Manifest present but no inputs / outputs / params declared._`);
    }
    return md;
}

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('xinsp2');
    // The configured port is the STARTING port; in managed mode the extension
    // bumps to the next free one so multiple projects each get their own backend.
    let port = config.get<number>('backendPort', 7823);
    const autoStart = config.get<boolean>('autoStartBackend', true);
    const extraPluginDirs = config.get<string[]>('extraPluginDirs', []);
    const remoteUrl = (config.get<string>('remoteUrl', '') || '').trim();
    const authSecret = (config.get<string>('authSecret', '') || '').trim();
    // Backend ownership mode (see docs/internals/fe-be.md):
    //   managed — extension spawns + respawns the backend (dev inner-loop).
    //   attach  — a supervisor (xinsp-fe.exe) owns it; connect read-only, never spawn.
    //   auto    — attach if a backend is already on the port, else managed.
    const backendMode = (config.get<string>('backendMode', 'managed') || 'managed').trim();
    // Remote mode: skip spawning a local backend, connect to the given URL.
    // Combine with authSecret to drive a backend started with --auth.
    const isRemote = remoteUrl.length > 0;
    attachMode = backendMode === 'attach';   // 'auto' may flip this after a port probe
    const wsUrl = isRemote ? remoteUrl : `ws://127.0.0.1:${port}`;
    const output = vscode.window.createOutputChannel('xInsp2');

    // PlatformIO-style project recognition: if the opened workspace folder IS an
    // xInsp2 project (a project.json that looks like ours — has script/instances/
    // params), remember it so the connect handler auto-opens AND auto-compiles it
    // ("open the folder -> it just runs"). A generic project.json (e.g. a Node
    // package) is ignored, so we don't spawn a backend for unrelated folders.
    let looksLikeXinspProject = false;
    let unrelatedProjectJson = false;   // a project.json that is NOT ours
    {
        const fs0 = require('fs');
        for (const wf of vscode.workspace.workspaceFolders ?? []) {
            const pj = path.join(wf.uri.fsPath, 'project.json');
            if (!fs0.existsSync(pj)) continue;
            try {
                const o = JSON.parse(fs0.readFileSync(pj, 'utf8'));
                if (Array.isArray(o.instances) || Array.isArray(o.params) || typeof o.script === 'string') {
                    lastProjectFolder = wf.uri.fsPath;
                    looksLikeXinspProject = true;
                    break;
                }
                unrelatedProjectJson = true;   // parses but isn't an xInsp2 project
            } catch { unrelatedProjectJson = true; /* unparseable — not ours */ }
        }
    }

    // Diagnostics from compile_and_load — drives Problems panel + squiggles.
    // Cleared and rebuilt on each compile (success or failure).
    const diagnostics = vscode.languages.createDiagnosticCollection('xinsp2');
    context.subscriptions.push(diagnostics);
    function applyDiagnostics(diags: any[] | undefined, sourceCpp: string) {
        diagnostics.clear();
        if (!Array.isArray(diags) || diags.length === 0) return;
        const baseDir = path.dirname(sourceCpp);
        const buckets = new Map<string, vscode.Diagnostic[]>();
        for (const d of diags) {
            if (!d || !d.file) continue;
            // cl.exe may print relative paths ("inspection.cpp") or absolute.
            const abs = path.isAbsolute(d.file) ? d.file : path.resolve(baseDir, d.file);
            const line = Math.max(0, (Number(d.line) || 1) - 1);
            const col  = Math.max(0, (Number(d.col)  || 1) - 1);
            // Highlight to end of token (word) by anchoring at col and
            // letting VS Code show a single squiggle at that position.
            const range = new vscode.Range(line, col, line, col + 1);
            const sev = d.severity === 'error'
                ? vscode.DiagnosticSeverity.Error
                : d.severity === 'warning'
                ? vscode.DiagnosticSeverity.Warning
                : vscode.DiagnosticSeverity.Information;
            const msg = (d.code ? `${d.code}: ` : '') + (d.message || '');
            const diag = new vscode.Diagnostic(range, msg, sev);
            diag.source = 'xinsp2';
            const arr = buckets.get(abs) ?? [];
            arr.push(diag);
            buckets.set(abs, arr);
        }
        for (const [file, arr] of buckets) {
            diagnostics.set(vscode.Uri.file(file), arr);
        }
    }
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
    setCtx('isActiveScript', false);  // active editor == open project's script?
    // 'busy' = we're auto-opening/compiling a recognized project. While true the
    // panel shows a "Starting…" message instead of the create/open welcome.
    setCtx('busy', looksLikeXinspProject);

    // Backend health indicator (leftmost). Toggles between connected + disconnected.
    const healthStatus = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 101);
    healthStatus.command = 'xinsp2.restartBackend';
    context.subscriptions.push(healthStatus);
    // Authoritative FE state, populated by the fe-status.json watcher below (attach
    // mode only). When present it replaces the "infer down from a WS drop" guess
    // with the supervisor's real state (recovering vs latched, reason, forensics).
    let latestFeStatus: any = undefined;
    const feTooltip = (s: any, lead: string): string => {
        const lines = [lead];
        if (s.reason) lines.push(`reason: ${s.reason}`);
        if (typeof s.respawn_max === 'number' && s.respawn_max > 0)
            lines.push(`respawns: ${s.consecutive ?? 0}/${s.respawn_max}`);
        const e = s.last_event;
        if (e && e.exception)
            lines.push(`last crash: ${e.exception}${e.module ? ' in ' + e.module : ''}`
                + `${e.phase ? ' @ ' + e.phase : ''}`);
        if (s.crash_history) lines.push(`history: ${s.crash_history}`);
        lines.push('The xinsp-fe supervisor owns this backend; the extension does not manage it.');
        return lines.join('\n');
    };
    const portTag = () => (backendPortInUse ? ` :${backendPortInUse}` : '');
    const updateHealthStatus = (connected: boolean) => {
        if (connected) {
            healthStatus.text = attachMode ? '$(plug) xInsp2 · attached' : ('$(zap) xInsp2' + portTag());
            healthStatus.tooltip = attachMode
                ? 'Attached to a backend managed by the xinsp-fe supervisor.'
                : `xInsp2 backend connected${backendPortInUse ? ' on port ' + backendPortInUse : ''}. Click to restart.`;
            healthStatus.backgroundColor = undefined;
        } else if (attachMode && latestFeStatus) {
            // Drive the indicator from the FE's TRUE state, not the WS drop.
            const s = latestFeStatus;
            if (s.latched) {
                // The FE gave up (RespawnLimitExceeded / comms gaveup) — stop
                // implying recovery; this needs a human.
                healthStatus.text = '$(error) xInsp2 · LATCHED';
                healthStatus.tooltip = feTooltip(s,
                    'Backend down and the supervisor gave up — manual restart required.');
                healthStatus.backgroundColor = new vscode.ThemeColor('statusBarItem.errorBackground');
            } else if (s.state === 'safe' || s.state === 'starting') {
                const budget = (s.respawn_max > 0) ? ` (${s.consecutive ?? 0}/${s.respawn_max})` : '';
                healthStatus.text = `$(shield) xInsp2 · safe${budget}`;
                healthStatus.tooltip = feTooltip(s,
                    'Backend down — the xinsp-fe supervisor is recovering it; line in safe state.');
                healthStatus.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
            } else {
                // state "stopped" (clean) or unknown — the line is simply down.
                healthStatus.text = '$(debug-disconnect) xInsp2 · offline';
                healthStatus.tooltip = feTooltip(s, 'Backend stopped (supervisor not recovering).');
                healthStatus.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
            }
        } else if (attachMode) {
            // No status file configured — fall back to inferring from the WS drop.
            healthStatus.text = '$(shield) xInsp2 · safe';
            healthStatus.tooltip = 'Backend down — the xinsp-fe supervisor is recovering it; '
                + 'the line is in its safe state. The extension does not manage this backend.';
            healthStatus.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
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
    // Absolute path of the open project's inspection script (from project.json's
    // `script` field — NOT a hardcoded filename, since projects pick their own).
    // Drives the editor-toolbar Compile/Run buttons via the xinsp2.isActiveScript
    // context key, so those buttons follow the real script whatever it's named.
    let currentScriptPath: string | undefined;
    // Resolve <folder>/<project.json script> to an absolute path. Falls back to
    // the DEFAULT_SCRIPT_NAME convention when project.json is missing/lacks it.
    const resolveScriptPath = (folder: string): string => {
        let scr = DEFAULT_SCRIPT_NAME;
        try {
            const pj = JSON.parse(require('fs').readFileSync(
                path.join(folder, 'project.json'), 'utf8'));
            if (typeof pj.script === 'string' && pj.script) scr = pj.script;
        } catch { /* use default */ }
        return path.join(folder, scr);
    };
    const SCRIPT_SRC_EXTS = ['.cpp', '.cc', '.cxx', '.hpp', '.hxx', '.h'];
    // Is `fsPath` part of the open project's SCRIPT side — i.e. the script itself
    // OR a sibling C/C++ file whose name is prefixed with the script's stem +"_"
    // (e.g. inspect.cpp → inspect_lane_a.hpp). Multi-file scripts split via
    // HEADERS #included into the one script TU (separate .cpp TUs can't link the
    // use()/thunk globals — see docs/guides/write-a-script.md). The stem prefix
    // makes the association explicit, so saving an unrelated header dropped in the
    // folder does NOT trigger a script recompile, and plugin sources (different
    // folder) never match.
    const isProjectScriptFile = (fsPath: string): boolean => {
        if (!currentScriptPath || !fsPath) return false;
        const rp  = path.resolve(fsPath).toLowerCase();
        const scr = path.resolve(currentScriptPath).toLowerCase();
        if (rp === scr) return true;                               // the script itself
        if (path.dirname(rp) !== path.dirname(scr)) return false;  // siblings only
        if (!SCRIPT_SRC_EXTS.includes(path.extname(rp))) return false;
        const stem = path.basename(scr, path.extname(scr));        // "inspect"
        return path.basename(rp).startsWith(stem + '_');           // "inspect_*"
    };
    // The editor-title Compile/Run buttons gate on this: true while the active
    // editor is the script OR one of its #included project files, so the buttons
    // stay available when you're editing a lane file, not just inspect.cpp.
    const refreshActiveScriptCtx = () => {
        const active = vscode.window.activeTextEditor?.document.uri.fsPath;
        setCtx('isActiveScript', !!active && isProjectScriptFile(active));
    };
    const setCurrentProject = (folder: string | undefined, name: string | undefined) => {
        currentProjectPath = folder;
        currentProjectName = name;
        currentScriptPath = folder ? resolveScriptPath(folder) : undefined;
        refreshActiveScriptCtx();
    };
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

    // Live component status (cmd:status snapshot on connect + `status` events).
    // Retained map; re-synced on every (re)connect so the latest always shows.
    const statusMap: Record<string, string> = {};
    const refreshStatuses = () => treeProvider.setStatuses({ ...statusMap });

    // Plugin data cache (the "Plugins" tree view was removed; the Plugin Browser
    // webview is the management surface now — this just holds the last-known set).
    const pluginRegistry = new PluginRegistry();
    pluginRegistry.setRemovableFolders(extraPluginDirs);

    // The Plugin Browser webview panel + helpers to (re)feed it live status. The
    // panel is created lazily by the xinsp2.pluginBrowser command; these are
    // shared so list_plugins refreshes can push fresh loaded/uses while it's open.
    let pluginBrowserPanel: vscode.WebviewPanel | undefined;
    const pbLive = (): PBLive => ({
        byName: new Map(pluginRegistry.listPlugins().map(p => [p.name, p])),
        uses: (n) => pluginRegistry.uses(n),
        isRemovable: (f) => pluginRegistry.isRemovable(f),
    });
    const refreshPluginBrowser = () => {
        if (lastProjectFolder)
            pluginBrowserPanel?.webview.postMessage({ type: 'model', model: pbBuildModel(lastProjectFolder, pbLive()) });
    };

    // Activity-bar / view badge — surfaces connection state visually.
    function setViewBadge(connected: boolean, instanceCount: number, pluginCount: number) {
        // Disconnected: red "!" badge.
        if (!connected) {
            instancesView.badge = { tooltip: 'xInsp2 backend offline', value: 1 };
            return;
        }
        instancesView.badge = instanceCount > 0
            ? { tooltip: `${instanceCount} instance(s)`, value: instanceCount }
            : undefined;
    }
    let lastInstanceCount = 0;
    let lastPluginCount = 0;
    // instance name -> plugin name, kept fresh from the `instances` message. Used
    // to colour xi::use("…") references in the script and to resolve the plugin
    // when Ctrl/⌘+clicking an instance to open its webui.
    const instanceMap = new Map<string, string>();
    // pluginName -> manifest object (the free-form schema from plugin.json's
    // `manifest` block: params / inputs / outputs / exchange). Fed from
    // list_plugins; used by the hover provider to show a plugin's I/O contract.
    const pluginManifests = new Map<string, any>();
    function cachePluginManifests(plugins: any[]) {
        for (const p of plugins || []) {
            if (p?.name && p.manifest !== undefined) pluginManifests.set(p.name, p.manifest);
        }
    }

    // Hover over the "name" in xi::use("name") in a script → show that instance's
    // plugin I/O contract (inputs/outputs/params from the plugin manifest), with
    // copyable code blocks. Falls through to the default C++ hover when the name
    // isn't a known instance or no project is open.
    context.subscriptions.push(
        vscode.languages.registerHoverProvider({ language: 'cpp', scheme: 'file' }, {
            provideHover(document, position) {
                const line = document.lineAt(position.line).text;
                const re = /\buse\s*(?:<[^>]*>)?\s*\(\s*"([^"]+)"\s*\)/g;
                let m: RegExpExecArray | null;
                while ((m = re.exec(line)) !== null) {
                    const start = m.index, end = m.index + m[0].length;
                    if (position.character < start || position.character > end) continue;
                    const name = m[1];
                    const plugin = instanceMap.get(name);
                    output.appendLine(`[hover] use("${name}") plugin=${plugin ?? '(unknown)'} manifest=${plugin && pluginManifests.has(plugin) ? 'yes' : 'no'}`);
                    const range = new vscode.Range(position.line, start, position.line, end);
                    if (!plugin) {
                        const md = new vscode.MarkdownString(
                            `**${name}** — not a known instance yet.\n\n` +
                            `Open the project + connect the backend so its plugin contract loads here.`);
                        return new vscode.Hover(md, range);
                    }
                    return new vscode.Hover(renderPluginHover(name, plugin, pluginManifests.get(plugin)), range);
                }
                return undefined;
            },
        }),
    );

    // ---- xi::use("…") helpers (instance highlight + Ctrl-click → webui) ------
    // group 1 = everything up to & including the opening quote; group 2 = the name.
    const USE_RE = /(xi::use\s*(?:<[^>]*>)?\s*\(\s*")([^"]+)"/g;
    function scanUses(doc: vscode.TextDocument): { name: string; range: vscode.Range }[] {
        const text = doc.getText(), out: { name: string; range: vscode.Range }[] = [];
        for (let m; (m = USE_RE.exec(text)); ) {
            const start = m.index + m[1].length;
            out.push({ name: m[2], range: new vscode.Range(doc.positionAt(start), doc.positionAt(start + m[2].length)) });
        }
        return out;
    }
    // ---- Pipeline graph (stage 1: nodes from use() scan, click → webui) -----
    // Collect the script's instance nodes in first-appearance order across the
    // script + its inspect_* sibling files. STAGE 1 IS NODES ONLY — edges (data
    // flow) need a runtime trace and aren't inferred statically (the script's
    // wiring is imperative C++: data is pulled out of Records, computed on, and
    // fed back in, so a byte-accurate dataflow can't be parsed out). Clicking a
    // node opens that instance's webui — the primary goal here.
    type PipelineNode = { kind: 'node'; name: string; plugin?: string; inputs: number; outputs: number; known: boolean };
    type PipelineVar  = { kind: 'var'; name: string };
    type PipelineItem = PipelineNode | PipelineVar;

    // The script + its inspect_* sibling files (script first). Falls back to the
    // active cpp editor when no project is open.
    function gatherScriptFiles(): string[] {
        const fsmod = require('fs') as typeof import('fs');
        const files: string[] = [];
        if (currentScriptPath) {
            files.push(currentScriptPath);
            const dir  = path.dirname(currentScriptPath);
            const stem = path.basename(currentScriptPath, path.extname(currentScriptPath)).toLowerCase();
            try {
                for (const f of fsmod.readdirSync(dir).sort()) {
                    const ext = path.extname(f).toLowerCase();
                    if (!f.toLowerCase().startsWith(stem + '_') || !SCRIPT_SRC_EXTS.includes(ext)) continue;
                    files.push(path.join(dir, f));
                }
            } catch {}
        }
        return files;
    }
    function gatherScriptTexts(): string[] {
        const fsmod = require('fs') as typeof import('fs');
        const texts: string[] = [];
        for (const f of gatherScriptFiles()) {
            try { texts.push(fsmod.readFileSync(f, 'utf8')); } catch {}
        }
        if (!texts.length) {
            const ed = vscode.window.activeTextEditor;
            if (ed?.document.languageId === 'cpp') texts.push(ed.document.getText());
        }
        return texts;
    }
    // Open the script file where `VAR(name, …)` / `EMIT(name)` is declared and
    // reveal it — the graph's VAR chips jump here.
    async function revealVarSite(name: string) {
        const fsmod = require('fs') as typeof import('fs');
        const esc = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        const re = new RegExp('\\b(?:VAR|VAR_RAW|EMIT|EMIT_RAW)\\s*\\(\\s*' + esc + '\\b');
        for (const f of gatherScriptFiles()) {
            let text = ''; try { text = fsmod.readFileSync(f, 'utf8'); } catch { continue; }
            const m = re.exec(text);
            if (!m) continue;
            const doc = await vscode.workspace.openTextDocument(f);
            const ed  = await vscode.window.showTextDocument(doc, vscode.ViewColumn.One);
            const pos = doc.positionAt(m.index);
            ed.selection = new vscode.Selection(pos, pos);
            ed.revealRange(new vscode.Range(pos, pos), vscode.TextEditorRevealType.InCenter);
            return;
        }
    }

    // Ordered graph elements in SOURCE order: plugin nodes (use("…")) interleaved
    // with the VAR()/EMIT() names the script surfaces between them. The VAR chips
    // ARE the script glue — the compute the composition layer does between plugin
    // stages, which can't be a plugin and isn't a traceable dataflow edge. Showing
    // them by source position is honest (no faked provenance) and useful.
    function extractPipelineItems(): PipelineItem[] {
        const useRe = new RegExp(USE_RE.source, 'g');                    // m[2] = instance name
        const varRe = /\b(?:VAR|VAR_RAW|EMIT|EMIT_RAW)\s*\(\s*([A-Za-z_]\w*)/g;
        const items: PipelineItem[] = [];
        const seenNode = new Set<string>(), seenVar = new Set<string>();
        for (const text of gatherScriptTexts()) {
            const hits: { idx: number; kind: 'node' | 'var'; name: string }[] = [];
            for (let m; (m = useRe.exec(text)); ) hits.push({ idx: m.index, kind: 'node', name: m[2] });
            for (let m; (m = varRe.exec(text)); ) hits.push({ idx: m.index, kind: 'var', name: m[1] });
            hits.sort((a, b) => a.idx - b.idx);
            for (const h of hits) {
                if (h.kind === 'node') {
                    if (seenNode.has(h.name)) continue; seenNode.add(h.name);
                    const plugin = instanceMap.get(h.name);
                    const manifest = plugin ? pluginManifests.get(plugin) : undefined;
                    items.push({ kind: 'node', name: h.name, plugin,
                        inputs:  Array.isArray(manifest?.inputs)  ? manifest.inputs.length  : 0,
                        outputs: Array.isArray(manifest?.outputs) ? manifest.outputs.length : 0,
                        known: !!plugin });
                } else {
                    if (seenVar.has(h.name)) continue; seenVar.add(h.name);
                    items.push({ kind: 'var', name: h.name });
                }
            }
        }
        return items;
    }
    // Nodes only (back-compat for callers that just want the instances).
    function extractPipelineNodes(): PipelineNode[] {
        return extractPipelineItems().filter((i): i is PipelineNode => i.kind === 'node');
    }

    let pipelineGraphPanel: vscode.WebviewPanel | undefined;
    type GraphEdge = { from: string; to: string; keys?: string[] };
    function renderPipelineGraphHtml(items: PipelineItem[], edges: GraphEdge[] = []): string {
        const esc = (s: string) => s.replace(/[&<>"]/g, c =>
            ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]!));
        const cards = items.map(it => it.kind === 'var'
            ? `<div class="var" data-var="${esc(it.name)}" title="Jump to VAR ${esc(it.name)} in the script">VAR ${esc(it.name)}</div>`
            : `
      <div class="node ${it.known ? '' : 'unknown'}" data-name="${esc(it.name)}" data-plugin="${esc(it.plugin || '')}">
        <div class="nm">${esc(it.name)}</div>
        <div class="pl">${it.plugin ? esc(it.plugin) : '(not a known instance)'}</div>
        ${it.known ? `<div class="io">${it.inputs} in · ${it.outputs} out</div>` : ''}
      </div>`).join('\n');
        const hasEdges = edges.length > 0;
        return `<!DOCTYPE html><html><head><meta charset="utf-8"><style>
      body { font-family: var(--vscode-font-family); color: var(--vscode-foreground);
             background: var(--vscode-editor-background); padding: 16px; }
      .hint { color: var(--vscode-descriptionForeground); font-size: 12px; margin-bottom: 16px; }
      .col { display: flex; flex-direction: column; align-items: center; }
      .node { min-width: 220px; border: 1px solid var(--vscode-widget-border, #8884);
              border-radius: 8px; padding: 10px 16px; cursor: pointer; text-align: center;
              background: var(--vscode-editorWidget-background); }
      .node:hover { border-color: var(--vscode-focusBorder); }
      .node.unknown { opacity: 0.6; border-style: dashed; }
      .nm { font-weight: 600; }
      .pl { color: var(--vscode-descriptionForeground); font-size: 12px; margin-top: 2px; }
      .io { color: var(--vscode-descriptionForeground); font-size: 11px; margin-top: 4px; }
      .col > * { margin: 4px 0; }
      .var { font-size: 11px; color: var(--vscode-descriptionForeground); cursor: pointer;
             border: 1px dashed var(--vscode-widget-border, #8884); border-radius: 10px;
             padding: 1px 8px; opacity: 0.8; }
      .var:hover { opacity: 1; border-color: var(--vscode-focusBorder); }
      .empty { color: var(--vscode-descriptionForeground); }
      .bar { margin-bottom: 12px; }
      button { font: inherit; padding: 4px 10px; cursor: pointer;
               background: var(--vscode-button-background); color: var(--vscode-button-foreground);
               border: none; border-radius: 4px; }
      #wrap { position: relative; }
      #edges { position: absolute; inset: 0; pointer-events: none; z-index: 5; overflow: visible; }
      .elabel { fill: var(--vscode-descriptionForeground); font-size: 10px; }
      .eline { stroke: var(--vscode-charts-blue, #4daafc); stroke-width: 1.5; fill: none; }
    </style></head><body>
      <div class="hint">Plugin nodes (click → webui) interleaved with the VAR chips the
        script computes between them (click a chip → jump to its code), in source order.
        <br>${hasEdges
          ? 'Arrows = observed image dataflow (last capture). Scalar/JSON flow through the VAR glue is not traced.'
          : 'Click <b>Capture dataflow</b> to run once and overlay image-dataflow edges.'}</div>
      <div class="bar"><button id="cap">⟳ Capture dataflow</button></div>
      <div id="wrap">
        <svg id="edges"></svg>
        <div class="col">
          ${items.length ? cards : '<div class="empty">No xi::use("…") instances found in the script.</div>'}
        </div>
      </div>
      <script>
        const vscode = acquireVsCodeApi();
        const EDGES = ${JSON.stringify(edges)};
        for (const el of document.querySelectorAll('.node')) {
          el.addEventListener('click', () => vscode.postMessage({
            type: 'openUI', name: el.dataset.name, plugin: el.dataset.plugin }));
        }
        // VAR chips = script glue → jump to the code where they're declared.
        for (const el of document.querySelectorAll('.var')) {
          el.addEventListener('click', () => vscode.postMessage({ type: 'goto', name: el.dataset.var }));
        }
        document.getElementById('cap').addEventListener('click', () => {
          document.getElementById('cap').textContent = '⟳ Capturing…';
          vscode.postMessage({ type: 'capture' });
        });
        // Draw edges as SVG connectors between node centers (positions measured
        // after layout, so it's robust to wrapping/spacing).
        function draw() {
          const svg = document.getElementById('edges');
          const wrap = document.getElementById('wrap').getBoundingClientRect();
          const at = (name) => {
            const el = document.querySelector('.node[data-name="' + name + '"]');
            if (!el) return null;
            const r = el.getBoundingClientRect();
            return { x: r.left - wrap.left + r.width / 2, top: r.top - wrap.top, bot: r.bottom - wrap.top };
          };
          let html = '';
          for (const e of EDGES) {
            const a = at(e.from), b = at(e.to); if (!a || !b) continue;
            const y1 = a.bot, y2 = b.top, mx = (a.x + b.x) / 2, my = (y1 + y2) / 2;
            html += '<path class="eline" marker-end="url(#arr)" d="M ' + a.x + ' ' + y1 +
                    ' C ' + a.x + ' ' + my + ' ' + b.x + ' ' + my + ' ' + b.x + ' ' + y2 + '"/>';
            const lbl = (e.keys || []).join(', ');
            if (lbl) html += '<text class="elabel" x="' + (mx + 6) + '" y="' + my + '">' + lbl + '</text>';
          }
          svg.innerHTML = '<defs><marker id="arr" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto">' +
                          '<path d="M0,0 L6,3 L0,6 Z" fill="var(--vscode-charts-blue,#4daafc)"/></marker></defs>' + html;
        }
        draw(); window.addEventListener('resize', draw);
      </script>
    </body></html>`;
    }

    // Run one inspection with dataflow capture on, then return the reconstructed
    // edges. OFF by default in the backend, so this enables → runs → snapshots →
    // disables. framePath is optional (frame-driven projects need it; source /
    // continuous projects don't).
    // First image file under <project>/frames — a sample frame so the capture
    // button works for frame-driven projects (imread current_frame_path) without
    // the user wiring one up.
    function firstProjectFrame(): string | undefined {
        if (!currentProjectPath) return undefined;
        try {
            const fsmod = require('fs') as typeof import('fs');
            const dir = path.join(currentProjectPath, 'frames');
            const f = fsmod.readdirSync(dir).filter(x => /\.(png|jpe?g|bmp|tiff?)$/i.test(x)).sort()[0];
            if (f) return path.join(dir, f).split('\\').join('/');
        } catch { /* no frames dir */ }
        return undefined;
    }
    async function captureGraphEdges(framePath?: string): Promise<{ ran: string[]; edges: GraphEdge[] }> {
        await sendCmd('graph_capture', { enable: true });
        try {
            if (g_continuous) {
                // Source / continuous-driven: frames are already flowing — just
                // observe a window of live dispatches, don't issue our own run.
                await new Promise(r => setTimeout(r, 900));
            } else {
                // Single-shot: run once. cmd:run returns immediately (ms:0) and
                // inspects on a detached thread, so wait before snapshotting.
                const fp = framePath || firstProjectFrame();
                await sendCmd('run', fp ? { frame_path: fp } : undefined);
                await new Promise(r => setTimeout(r, 700));
            }
            const snap = await sendCmd('graph_snapshot');
            return { ran: snap?.data?.ran ?? [], edges: snap?.data?.edges ?? [] };
        } finally {
            await sendCmd('graph_capture', { enable: false });
        }
    }

    // Capture a dataflow run and repaint the open graph panel with the edges
    // (what the webview's "⟳ Capture dataflow" button does). Returns the edges.
    async function captureAndRenderGraph(): Promise<GraphEdge[]> {
        let edges: GraphEdge[] = [];
        try { edges = (await captureGraphEdges()).edges; }
        catch (e: any) { output.appendLine(`[graph] capture failed: ${e?.message || e}`); }
        if (pipelineGraphPanel)
            pipelineGraphPanel.webview.html = renderPipelineGraphHtml(extractPipelineItems(), edges);
        return edges;
    }

    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openPipelineGraph', () => {
            if (!pipelineGraphPanel) {
                pipelineGraphPanel = vscode.window.createWebviewPanel(
                    'xinsp2.pipelineGraph', 'Pipeline Graph',
                    vscode.ViewColumn.Beside, { enableScripts: true });
                pipelineGraphPanel.onDidDispose(() => { pipelineGraphPanel = undefined; });
                pipelineGraphPanel.webview.onDidReceiveMessage(async (msg: any) => {
                    if (msg?.type === 'openUI' && msg.name) {
                        const plugin = msg.plugin || instanceMap.get(msg.name);
                        vscode.commands.executeCommand('xinsp2.openInstanceUI', msg.name, plugin);
                    } else if (msg?.type === 'goto' && msg.name) {
                        revealVarSite(msg.name);
                    } else if (msg?.type === 'capture') {
                        await captureAndRenderGraph();
                    }
                });
            }
            pipelineGraphPanel.webview.html = renderPipelineGraphHtml(extractPipelineItems());
            pipelineGraphPanel.reveal(vscode.ViewColumn.Beside);
        }),
    );

    const knownDeco = vscode.window.createTextEditorDecorationType({
        color: new vscode.ThemeColor('charts.green'), fontWeight: 'bold',
        textDecoration: 'underline dotted',
    });
    const unknownDeco = vscode.window.createTextEditorDecorationType({
        color: new vscode.ThemeColor('errorForeground'),
        textDecoration: 'underline wavy var(--vscode-errorForeground)',
    });
    function refreshInstanceDecorations(editor?: vscode.TextEditor) {
        const ed = editor ?? vscode.window.activeTextEditor;
        if (!ed || ed.document.languageId !== 'cpp') return;
        const known: vscode.Range[] = [], unknown: vscode.Range[] = [];
        const haveList = instanceMap.size > 0;
        for (const u of scanUses(ed.document)) {
            (!haveList || instanceMap.has(u.name) ? known : unknown).push(u.range);
        }
        ed.setDecorations(knownDeco, known);
        ed.setDecorations(unknownDeco, unknown);
    }
    let decoTimer: any;
    context.subscriptions.push(
        knownDeco, unknownDeco,
        // Ctrl/⌘+click an instance name in xi::use("…") → open its webui.
        vscode.languages.registerDocumentLinkProvider({ language: 'cpp' }, {
            provideDocumentLinks(doc) {
                return scanUses(doc)
                    .filter((u) => instanceMap.has(u.name))
                    .map((u) => {
                        const link = new vscode.DocumentLink(u.range,
                            vscode.Uri.parse(`command:xinsp2.openInstanceUI?${encodeURIComponent(JSON.stringify([u.name]))}`));
                        link.tooltip = `Open “${u.name}” webui (Ctrl/⌘+click)`;
                        return link;
                    });
            },
        }),
        vscode.window.onDidChangeActiveTextEditor((ed) => {
            refreshInstanceDecorations(ed);
            refreshActiveScriptCtx();   // editor-title Compile/Run track the script
        }),
        vscode.workspace.onDidChangeTextDocument((e) => {
            const ed = vscode.window.activeTextEditor;
            if (ed && e.document === ed.document) { clearTimeout(decoTimer); decoTimer = setTimeout(() => refreshInstanceDecorations(), 200); }
        }),
    );
    refreshInstanceDecorations();
    let lastConnected = false;

    // ---- FE status channel (attach mode) -----------------------------------
    // The xinsp-fe supervisor rewrites a small fe-status.json on every transition
    // (see docs/internals/fe-be.md). When the path is configured we poll it and
    // drive the health indicator from the supervisor's TRUE state instead of
    // inferring "down" from a WS disconnect. Poll (not fs.watch): the FE writes via
    // atomic rename, which fs.watch reports unreliably across editors; the file is
    // tiny. Only meaningful while disconnected — a live WS is authoritative.
    const feStatusPath = (config.get<string>('feStatusFile', '') || '').trim();
    if (feStatusPath) {
        output.appendLine(`[xinsp2] FE status channel: ${feStatusPath}`);
        const fsmod = require('fs') as typeof import('fs');
        const readFeStatus = () => {
            try {
                latestFeStatus = JSON.parse(fsmod.readFileSync(feStatusPath, 'utf8'));
            } catch { /* missing / mid-write — keep the last good snapshot */ }
            if (!lastConnected) updateHealthStatus(false);
        };
        readFeStatus();
        const feIv = setInterval(readFeStatus, 1500);
        context.subscriptions.push({ dispose: () => clearInterval(feIv) });
    }

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

    // Preview subscription: the backend sends no image preview unless subscribed,
    // so subscribe to the viewer's image vars only while the viewer is visible
    // and unsubscribe when it's hidden. Re-asserted after each vars message and on
    // visibility change; we dedupe so we only hit the wire when the set changes.
    let sentImageSub = '';   // '' (unknown) | 'all' | 'none'
    // (subscribe ALL while the viewer is visible, none while hidden)
    const reconcileImageSubs = () => {
        const want = viewerProvider.visible ? 'all' : 'none';
        if (want === sentImageSub) return;
        sentImageSub = want;
        if (client?.connected)
            sendCmd('subscribe', want === 'all' ? { all: true } : { names: [] }).catch(() => {});
    };
    context.subscriptions.push(viewerProvider.onDidChangeVisibility.event(() => reconcileImageSubs()));

    // Thumbnail shift-click / double-click in the Viewer panel pops the
    // rich image viewer (pan + cursor-anchored zoom + pick tools).
    context.subscriptions.push(viewerProvider.onMessage.event(async (m: any) => {
        if (m && m.type === 'openInteractive' && m.src) {
            // src is a data URL like "data:image/jpeg;base64,...". Strip
            // the prefix so we can re-pack into ImageMessage shape.
            const i = String(m.src).indexOf('base64,');
            if (i < 0) return;
            const b64 = String(m.src).slice(i + 7);
            // We don't get width/height here; ImageViewerPanel will
            // discover them from the bitmap once it decodes. Stub w/h
            // are overwritten on bitmap load.
            const ImageViewerPanelMod = require('./imageViewerPanel');
            ImageViewerPanelMod.ImageViewerPanel.show(context.extensionUri, {
                name: m.name || 'image', width: 0, height: 0, jpegBase64: b64,
            });
        }
    }));
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
        // Fresh connection ⇒ backend forgot our preview subscription; re-assert.
        sentImageSub = '';
        reconcileImageSubs();
        setViewBadge(true, lastInstanceCount, lastPluginCount);
        // Pull the plugin list as soon as we're up (feeds the Plugin Browser).
        sendCmd('list_plugins').then((r: any) => {
            if (r?.ok && Array.isArray(r.data)) {
                pluginRegistry.update(r.data as PluginInfo[]);
                setCtx('hasPlugins', r.data.length > 0);
                refreshPluginBrowser();
            }
        }).catch(() => {});
        // Snapshot every component's latest status on (re)connect — this re-pull
        // over the backend's retained map is the delivery guarantee (the
        // `status` event below is just a live accelerator). Reset first so a
        // stale entry from a previous backend doesn't linger.
        for (const k of Object.keys(statusMap)) delete statusMap[k];
        sendCmd('status').then((r: any) => {
            const data = r?.data || {};
            for (const src of Object.keys(data)) statusMap[src] = data[src]?.text ?? '';
            refreshStatuses();
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
            setCtx('busy', true);   // opening + compiling → show "Starting…", not the welcome
            output.appendLine(`[xinsp2] restoring project: ${lastProjectFolder}`);
            sendCmd('open_project', { folder: lastProjectFolder }).then((r: any) => {
                if (r?.ok) {
                    setCtx('hasProject', true);
                    setCurrentProject(lastProjectFolder!, r.data?.name || path.basename(lastProjectFolder!));
                    setCtx('hasInstances', (r.data?.instances?.length ?? 0) > 0);
                    sendCmd('list_instances');
                    treeProvider.setProjectOpen(true, currentScriptPath ? path.basename(currentScriptPath) : undefined);
                    updateProjectStatus();
                    vscode.window.setStatusBarMessage('xInsp2: project restored', 3000);
                    // Auto-compile the project's script so the pipeline is LIVE
                    // (and the trigger sink installed) without a manual Compile —
                    // otherwise issuing/replaying a frame does nothing.
                    try {
                        const fsx = require('fs');
                        const pj = JSON.parse(fsx.readFileSync(path.join(lastProjectFolder!, 'project.json'), 'utf8'));
                        const scr = pj.script || DEFAULT_SCRIPT_NAME;
                        const scrPath = path.join(lastProjectFolder!, scr);
                        if (fsx.existsSync(scrPath)) {
                            output.appendLine(`[xinsp2] auto-compiling project script ${scr}`);
                            sendCmd('compile_and_load', { path: scrPath })
                                .then(() => { output.appendLine('[xinsp2] project script loaded — ready to run'); setCtx('busy', false); })
                                .catch((e: any) => { output.appendLine(`[xinsp2] auto-compile failed: ${e?.message || e}`); setCtx('busy', false); });
                        } else {
                            setCtx('busy', false);   // no script to compile — ready
                        }
                    } catch (e: any) {
                        output.appendLine(`[xinsp2] auto-compile skipped: ${e?.message || e}`);
                        setCtx('busy', false);
                    }
                } else {
                    output.appendLine(`[xinsp2] could not restore project: ${r?.error || 'unknown'}`);
                    setCtx('busy', false);
                }
            }).catch((e) => { output.appendLine(`[xinsp2] restore error: ${e?.message || e}`); setCtx('busy', false); });
        }
    });

    client.on('close', () => {
        output.appendLine('[xinsp2] disconnected');
        setCtx('connected', false);
        setCtx('hasProject', false);
        setCtx('hasInstances', false);
        setCtx('busy', false);   // not loading anymore (avoids a stuck "Starting…")
        setCurrentProject(undefined, undefined);
        updateProjectStatus();
        updateHealthStatus(false);
        treeProvider.setProjectOpen(false);
        lastConnected = false;
        setViewBadge(false, 0, 0);
        if (attachMode) {
            // The FE owns recovery; make clear the extension isn't going to
            // respawn. Word it from the FE's true state when we have it: a latched
            // supervisor isn't "recovering" — it needs a human.
            const msg = (latestFeStatus && latestFeStatus.latched)
                ? `xInsp2: backend down — supervisor gave up (${latestFeStatus.reason || 'RespawnLimitExceeded'}); manual restart required`
                : 'xInsp2: backend down — xinsp-fe supervisor recovering (line in safe state)';
            vscode.window.setStatusBarMessage(msg, 6000);
        }
    });

    client.on('json', (msg: any) => {
        if (msg.type === 'event' && msg.name === 'hello') {
            output.appendLine(`[xinsp2] backend v${msg.data?.version}`);
        } else if (msg.type === 'event' && msg.name === 'status') {
            // Live status delta — accelerator between the connect snapshots.
            const src = msg.data?.source;
            if (typeof src === 'string') {
                statusMap[src] = msg.data?.text ?? '';
                refreshStatuses();
            }
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
            // Keep the name->plugin map + script highlighting in sync.
            instanceMap.clear();
            for (const i of (msg.instances ?? [])) if (i.name && i.plugin) instanceMap.set(i.name, i.plugin);
            refreshInstanceDecorations();
            setCtx('hasInstances', (msg.instances?.length ?? 0) > 0);
            lastInstanceCount = (msg.instances?.length ?? 0);
            setViewBadge(lastConnected, lastInstanceCount, lastPluginCount);
            // Recount plugin usage.
            const uses = new Map<string, number>();
            for (const i of (msg.instances ?? [])) {
                uses.set(i.plugin, (uses.get(i.plugin) || 0) + 1);
            }
            // Keep the current set but refresh the use counts.
            pluginRegistry.update(pluginRegistry.listPlugins() as PluginInfo[], uses);
            // Fire-and-forget refetch in case a plugin got loaded.
            sendCmd('list_plugins').then((r: any) => {
                if (r?.ok && Array.isArray(r.data)) {
                    pluginRegistry.update(r.data as PluginInfo[], uses);
                    cachePluginManifests(r.data);   // for the use("…") hover I/O contract
                    setCtx('hasPlugins', r.data.length > 0);
                    refreshPluginBrowser();         // live loaded/uses → browser, if open
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
            // Also remember the most-recent preview so the
            // xinsp2.openImageViewer command can re-show it in the
            // rich viewer (with pan/zoom + pick tools).
            lastPreview = { gid, width, height, b64,
                            name: lastPreviewName.get(gid) || `gid:${gid}` };
        }
    });

    // Cache the most-recent preview message so the user can pop it
    // open in the interactive viewer after-the-fact.
    let lastPreview: { gid: number; width: number; height: number; b64: string; name: string } | null = null;
    const lastPreviewName = new Map<number, string>();

    // Expose openImageViewer to the user — palette + later wired into
    // viewer thumbnails. Picks the most recent image if no arg given.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openImageViewer', (arg?: { gid?: number, name?: string, width?: number, height?: number, jpeg?: string }) => {
            const src = arg && arg.jpeg
                ? { name: arg.name || 'image', width: arg.width!, height: arg.height!, jpegBase64: arg.jpeg }
                : (lastPreview
                    ? { name: lastPreview.name, width: lastPreview.width, height: lastPreview.height, jpegBase64: lastPreview.b64 }
                    : null);
            if (!src) {
                vscode.window.showInformationMessage('No image to view yet — run an inspection or preview an instance first.');
                return;
            }
            ImageViewerPanel.show(context.extensionUri, src);
        }),
    );

    // Surface picks so other extensions / scripts can react. We just
    // log + show a toast; future hooks could write a ROI param.
    context.subscriptions.push(ImageViewerPanel.onPick.event((p) => {
        const text = p.tool === 'point'
            ? `Picked point (${p.x}, ${p.y}) on ${p.image}`
            : `Picked rect (${p.x}, ${p.y}) ${p.w}×${p.h} on ${p.image}`;
        output.appendLine('[xinsp2] ' + text);
        vscode.window.setStatusBarMessage('xInsp2: ' + text, 4000);
    }));

    // Test hook: run the image viewer's pan/zoom invariants. Used by
    // the e2e journey to validate cursor-anchored zoom math without
    // needing real mouse events. Returns the next selftest_result via
    // a one-shot promise.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.imageViewer.runSelftest', async () => {
            return new Promise((resolve) => {
                const sub = ImageViewerPanel.onSelftest.event((r) => {
                    sub.dispose();
                    resolve(r);
                });
                if (!ImageViewerPanel.runSelftest()) {
                    sub.dispose();
                    resolve({ ok: false, steps: [{ label: 'no panel open', ok: false }] });
                }
            });
        }),
    );

    // Test hook: drive a discrete pan/zoom op so journeys can
    // screenshot between operations. See ImageViewerPanel.applyOp.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.imageViewer.applyOp', async (op: any) => {
            return ImageViewerPanel.applyOp(op || {});
        }),
    );

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
            // First run: reveal the Viewer so its webview resolves and the vars
            // (which arrive async) have somewhere to land. Only when it hasn't
            // been opened yet, so we don't steal focus on every iteration.
            if (!viewerProvider.isResolved()) {
                try { await vscode.commands.executeCommand('xinsp2.viewer.focus'); } catch {}
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
                applyDiagnostics(rsp.data?.diagnostics, filePath);
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

    // --- Rebuild cmake/prebuilt plugins (external libs / CUDA) ---
    // For every `build: cmake` plugin whose source changed, the backend unloads
    // it, runs its own CMake build, then reloads the DLL and restores instance
    // state. Unchanged plugins are skipped. See docs/guides/write-a-plugin.md
    // (External libraries & CUDA).
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.rebuildPlugins', async (arg?: any) => {
            if (!client?.connected) { vscode.window.showWarningMessage('xInsp2: not connected'); return; }
            // From the per-item right-click the arg is a plugin tree Node — scope
            // the rebuild to just that plugin. From the title-bar button / palette
            // there's no arg → rebuild all cmake plugins (changed ones).
            const one: string | undefined = arg?.info?.name;
            const cmdArgs = one ? { plugins: [one] } : undefined;
            output.appendLine(one ? `[xinsp2] rebuilding plugin '${one}'...` : '[xinsp2] rebuilding cmake plugins...');
            await vscode.window.withProgress(
                { location: vscode.ProgressLocation.Notification,
                  title: one ? `xInsp2: Rebuilding ${one}…` : 'xInsp2: Rebuilding plugins…' },
                async () => {
                    try {
                        const rsp = await sendCmd('rebuild_plugins', cmdArgs);
                        const items: any[] = rsp.data?.plugins ?? [];
                        if (!items.length) {
                            vscode.window.showInformationMessage('xInsp2: no cmake/prebuilt plugins to rebuild.');
                            return;
                        }
                        for (const i of items) {
                            output.appendLine(`[xinsp2]   ${i.plugin}: ${i.status}${i.detail ? ' — ' + i.detail : ''}`);
                        }
                        const rebuilt = items.filter((i) => i.status === 'rebuilt');
                        const failed = items.filter((i) => i.status === 'failed');
                        if (failed.length) {
                            vscode.window.showErrorMessage(`xInsp2: ${failed.length} plugin(s) failed to rebuild — see Output.`);
                        } else {
                            vscode.window.setStatusBarMessage(`xInsp2: rebuilt ${rebuilt.length} plugin(s)`, 4000);
                        }
                        sendCmd('list_instances').catch(() => {});
                    } catch (e: any) {
                        vscode.window.showErrorMessage(`xInsp2 rebuild plugins: ${e.message}`);
                    }
                });
        })
    );

    // --- Plugin Browser: the single plugin-management surface (replaces the old
    //     "Plugins" tree). Browse the plugin_dirs roots + the declared (added)
    //     plugins with live loaded/uses; add/remove/toggle-compile write
    //     project.json and reopen; Rebuild/Export/Reveal act on a project plugin;
    //     Reveal/Remove on a search root; "+ Add folder" appends one. ---
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.pluginBrowser', async () => {
            if (!lastProjectFolder) { vscode.window.showWarningMessage('xInsp2: open a project first'); return; }
            const fs = require('fs') as typeof import('fs');
            const refresh = refreshPluginBrowser;
            const writePj = (mut: (pj: any) => void) => {
                const pjPath = path.join(lastProjectFolder!, 'project.json');
                let pj: any = {};
                try { pj = JSON.parse(fs.readFileSync(pjPath, 'utf8')); } catch { /* ignore */ }
                if (!pj.plugins || typeof pj.plugins !== 'object' || Array.isArray(pj.plugins)) pj.plugins = {};
                mut(pj);
                fs.writeFileSync(pjPath, JSON.stringify(pj, null, 2) + '\n');
            };
            const reopen = async () => {
                if (client?.connected) await sendCmd('open_project', { folder: lastProjectFolder });
                refresh();
            };
            if (pluginBrowserPanel) { pluginBrowserPanel.reveal(vscode.ViewColumn.Active); refresh(); return; }
            pluginBrowserPanel = vscode.window.createWebviewPanel(
                'xinsp2.pluginBrowser', 'Plugin Browser', vscode.ViewColumn.Active,
                { enableScripts: true, retainContextWhenHidden: true });
            pluginBrowserPanel.onDidDispose(() => { pluginBrowserPanel = undefined; });
            pluginBrowserPanel.webview.onDidReceiveMessage(async (msg: any) => {
                try {
                    if (msg.type === 'refresh') { refresh(); return; }
                    if (msg.type === 'add') {
                        const label = String(msg.path).split('/').pop()!;
                        writePj((pj) => { pj.plugins[label] = { path: msg.path, compile: !!msg.compile }; });
                        await reopen();
                    } else if (msg.type === 'remove') {
                        writePj((pj) => { delete pj.plugins[msg.label]; });
                        await reopen();
                    } else if (msg.type === 'toggleCompile') {
                        writePj((pj) => { if (pj.plugins[msg.label]) pj.plugins[msg.label].compile = !pj.plugins[msg.label].compile; });
                        await reopen();
                    } else if (msg.type === 'addFolder') {
                        const uris = await vscode.window.showOpenDialog({
                            canSelectFolders: true, canSelectFiles: false, canSelectMany: false,
                            openLabel: 'Add as plugin search root', title: 'Pick a folder to add to plugin_dirs' });
                        if (!uris || !uris.length) return;
                        const rel = path.relative(lastProjectFolder!, uris[0].fsPath);
                        const stored = (rel && !rel.startsWith('..') && !path.isAbsolute(rel))
                            ? './' + rel.split(path.sep).join('/')
                            : uris[0].fsPath.split(path.sep).join('/');
                        writePj((pj) => {
                            let dirs: string[] = Array.isArray(pj.plugin_dirs) ? pj.plugin_dirs : [];
                            if (!dirs.length) dirs = ['./plugins'];   // materialize the fallback once customizing
                            if (!dirs.includes(stored)) dirs.push(stored);
                            pj.plugin_dirs = dirs;
                        });
                        await reopen();
                    } else if (msg.type === 'removeFolder') {
                        // Drop a search root from project.json plugin_dirs (matched by
                        // resolved path, so the "(default)" suffix on raw is harmless).
                        writePj((pj) => {
                            const dirs: string[] = Array.isArray(pj.plugin_dirs) ? pj.plugin_dirs : [];
                            pj.plugin_dirs = dirs.filter((d) =>
                                path.resolve(pbExpandRoot(d, lastProjectFolder!)).toLowerCase()
                                !== path.resolve(String(msg.resolved)).toLowerCase());
                        });
                        await reopen();
                    } else if (msg.type === 'reveal') {
                        if (msg.path) try { await vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(String(msg.path))); } catch { /* ignore */ }
                    } else if (msg.type === 'rebuild') {
                        await vscode.commands.executeCommand('xinsp2.rebuildPlugins', { info: { name: msg.name } });
                        refresh();
                    } else if (msg.type === 'export') {
                        await vscode.commands.executeCommand('xinsp2.exportProjectPlugin', { info: { name: msg.name, origin: 'project' } });
                    }
                } catch (e: any) { vscode.window.showErrorMessage('xInsp2 Plugin Browser: ' + e.message); }
            });
            pluginBrowserPanel.webview.html = renderPluginBrowserHtml(pbBuildModel(lastProjectFolder, pbLive()));
        })
    );

    // --- Create a new project plugin from a template ---
    // Walks: pick template → enter name → optional description →
    // generate folder + plugin.cpp + plugin.json under
    // <project>/plugins/<name>/src/, then re-open the project so the
    // backend compiles + loads the new plugin. The .cpp opens in the
    // editor immediately so the user sees their new file.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.createProjectPlugin', async () => {
            if (!client?.connected) { vscode.window.showWarningMessage('xInsp2: not connected'); return; }
            if (!lastProjectFolder) {
                vscode.window.showWarningMessage('xInsp2: open a project first (xInsp2: Open Project).');
                return;
            }
            // 1. Template
            const tplPick = await vscode.window.showQuickPick(
                TEMPLATE_CHOICES.map(c => ({
                    label: c.label, description: c.description, detail: c.detail, id: c.id,
                })),
                { placeHolder: 'Pick a starter template — go from Easy upward', matchOnDetail: true });
            if (!tplPick) return;
            const tplId = (tplPick as any).id as TemplateId;

            // 2. Name (must be valid folder + C++ identifier-ish)
            const pname = await vscode.window.showInputBox({
                prompt: 'Plugin name (folder + display)',
                placeHolder: 'e.g. my_filter',
                validateInput: (v) => {
                    if (!v) return 'name is required';
                    if (!/^[A-Za-z][A-Za-z0-9_-]*$/.test(v)) return 'use letters / digits / _ / -, start with a letter';
                    return null;
                },
            });
            if (!pname) return;

            // 3. Description (optional)
            const pdesc = (await vscode.window.showInputBox({
                prompt: 'Short description (shown in the Plugin Browser, optional)',
                placeHolder: 'Leave blank to use the template name',
            })) || '';

            // 4. Materialize files. Project plugins live at
            //    <project>/plugins/<name>/{plugin.json, src/plugin.cpp}.
            const root = path.join(lastProjectFolder, 'plugins', pname);
            const srcDir = path.join(root, 'src');
            try {
                const fs = require('fs') as typeof import('fs');
                if (fs.existsSync(root)) {
                    const ow = await vscode.window.showWarningMessage(
                        `${pname} already exists. Overwrite plugin.cpp + plugin.json?`,
                        { modal: true }, 'Overwrite');
                    if (ow !== 'Overwrite') return;
                }
                // Render via the SDK's shared template machinery so this
                // path produces byte-identical output to the SDK CLI.
                // SDK is auto-located alongside the extension or backend.
                const sdkRoot = locateSdkRoot(
                    context.extensionPath,
                    /*backendExePath*/ findBackendExe(context),
                    vscode.workspace.getConfiguration('xinsp2').get<string>('sdkPath'));
                const files = await renderPluginFiles(sdkRoot, tplId, pname,
                    pdesc || `${tplId} template plugin: ${pname}`);
                for (const [rel, content] of files) {
                    const full = path.join(root, rel);
                    fs.mkdirSync(path.dirname(full), { recursive: true });
                    fs.writeFileSync(full, content);
                }
                const cppPath = path.join(root, 'src', 'plugin.cpp');

                // 5. Declare it in project.json so the declarative loader picks it
                //    up (./plugins is the default search root; compile:true builds +
                //    trusts it, matching the old auto-discovery behaviour).
                try {
                    const pjPath = path.join(lastProjectFolder, 'project.json');
                    const pj = JSON.parse(fs.readFileSync(pjPath, 'utf8'));
                    if (!pj.plugins || typeof pj.plugins !== 'object' || Array.isArray(pj.plugins)) pj.plugins = {};
                    pj.plugins[pname] = { path: pname, compile: true };
                    fs.writeFileSync(pjPath, JSON.stringify(pj, null, 2) + '\n');
                } catch (e: any) {
                    output.appendLine(`[xinsp2] warning: could not declare '${pname}' in project.json — ${e.message}`);
                }

                // 6. Re-open the project so the backend compiles + loads
                //    the new plugin. We use open_project rather than
                //    recompile_project_plugin because the plugin is
                //    brand-new and not yet in plugins_.
                output.appendLine(`[xinsp2] created project plugin '${pname}' (${tplId} template)`);
                const rsp = await sendCmd('open_project', { folder: lastProjectFolder });
                if (rsp.ok) {
                    pluginRegistry.update(rsp.data?.plugins || []);
                    refreshPluginBrowser();
                }

                // 6. Reveal the .cpp so the user sees their new code.
                const doc = await vscode.workspace.openTextDocument(cppPath);
                await vscode.window.showTextDocument(doc, { preview: false });
                vscode.window.showInformationMessage(
                    `xInsp2: created plugin '${pname}' — edit and save to recompile live.`);
            } catch (e: any) {
                vscode.window.showErrorMessage(`xInsp2: create plugin failed — ${e.message}`);
                output.appendLine(`[xinsp2] create plugin error: ${e.stack || e}`);
            }
        })
    );

    // --- Export a project plugin as a deployable folder ---
    // Picks any project-origin plugin from the current registry, asks for
    // the output dir, calls the backend which: (1) compiles Release with
    // PDB, (2) copies plugin.json + DLL + optional ui/ into <dest>/<name>/.
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.exportProjectPlugin', async (arg?: any) => {
            if (!client?.connected) { vscode.window.showWarningMessage('xInsp2: not connected'); return; }
            // Either invoked from tree context (passes a node with .info)
            // or via command palette (no arg → pick from list).
            let pname: string | undefined =
                arg?.info?.name && arg?.info?.origin === 'project'
                    ? arg.info.name : undefined;
            if (!pname) {
                const projPlugins = pluginRegistry.listPlugins()
                    .filter(p => p.origin === 'project');
                if (projPlugins.length === 0) {
                    vscode.window.showInformationMessage('No project plugins to export. Add one under <project>/plugins/.');
                    return;
                }
                const pick = await vscode.window.showQuickPick(
                    projPlugins.map((p: any) => ({ label: p.name, description: p.description })),
                    { placeHolder: 'Pick a project plugin to export' });
                if (!pick) return;
                pname = pick.label;
            }
            const destUri = await vscode.window.showOpenDialog({
                canSelectFolders: true, canSelectFiles: false,
                openLabel: 'Export here',
            });
            if (!destUri || destUri.length === 0) return;
            const dest = destUri[0].fsPath;
            output.appendLine(`[xinsp2] exporting project plugin '${pname}' → ${dest}...`);
            try {
                const rsp = await sendCmd('export_project_plugin', { plugin: pname, dest });
                if (rsp.ok) {
                    const d = rsp.data || {};
                    const msg = `xInsp2: exported '${pname}' to ${d.dest}`;
                    output.appendLine('[xinsp2] ' + msg);
                    const open = await vscode.window.showInformationMessage(msg, 'Reveal');
                    if (open === 'Reveal' && d.dest) {
                        vscode.commands.executeCommand('revealFileInOS', vscode.Uri.file(d.dest));
                    }
                    // DM-11: run the UI convention linter (warn-only) on the source
                    // plugin and surface findings in Output — never blocks the export.
                    try {
                        const fsmod = require('fs') as typeof import('fs');
                        const sdkRoot = locateSdkRoot(context.extensionPath, findBackendExe(context),
                            vscode.workspace.getConfiguration('xinsp2').get<string>('sdkPath'));
                        const lint = sdkRoot ? path.join(sdkRoot, 'testing', 'lint_plugin_ui.mjs') : '';
                        const projForSrc = lastProjectFolder;
                        const src = (projForSrc && pname) ? path.join(projForSrc, 'plugins', pname) : '';
                        if (lint && fsmod.existsSync(lint) && src && fsmod.existsSync(src)) {
                            const { spawnSync } = require('child_process');
                            const r = spawnSync(process.execPath, [lint, src],
                                { encoding: 'utf8', env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' } });
                            const txt = ((r.stdout || '') + (r.stderr || '')).trim();
                            if (txt) output.appendLine('[xinsp2] ui-lint: ' + txt.split('\n').join('\n           '));
                        }
                    } catch { /* lint is advisory */ }
                } else {
                    output.appendLine(`[xinsp2] export FAILED: ${rsp.error}`);
                    vscode.window.showErrorMessage(`xInsp2 export failed: ${rsp.error}`);
                }
            } catch (e: any) {
                vscode.window.showErrorMessage(`xInsp2 export: ${e.message}`);
            }
        })
    );

    // --- Recording / Replay (closes a UI gap exposed by audit) ---------
    // Backend has cmd:recording_{start,stop,replay,status} but until now
    // they were only reachable via the WS protocol. These commands give
    // the user proper UI entry points: file pickers + status feedback.
    let recordingFolder: string | null = null;
    const recordingStatus = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Left, 42);
    recordingStatus.command = 'xinsp2.stopRecording';
    context.subscriptions.push(recordingStatus);
    function showRecordingStatus(folder: string | null) {
        if (folder) {
            recordingStatus.text = `$(circle-filled) REC ${path.basename(folder)}`;
            recordingStatus.tooltip = `Recording trigger events to ${folder}\nClick to stop.`;
            recordingStatus.color = new vscode.ThemeColor('errorForeground');
            recordingStatus.show();
        } else {
            recordingStatus.hide();
        }
    }
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.startRecording', async () => {
            if (!client?.connected) { vscode.window.showWarningMessage('xInsp2: not connected'); return; }
            const dest = await vscode.window.showOpenDialog({
                canSelectFolders: true, canSelectFiles: false,
                openLabel: 'Record into this folder',
            });
            if (!dest || dest.length === 0) return;
            const folder = dest[0].fsPath;
            const rsp = await sendCmd('recording_start', { folder });
            if (rsp.ok) {
                recordingFolder = folder;
                showRecordingStatus(folder);
                vscode.window.showInformationMessage(`xInsp2: recording → ${folder}`);
            } else {
                vscode.window.showErrorMessage(`xInsp2: recording_start failed — ${rsp.error}`);
            }
        }),
        vscode.commands.registerCommand('xinsp2.stopRecording', async () => {
            if (!client?.connected) return;
            const rsp = await sendCmd('recording_stop');
            recordingFolder = null;
            showRecordingStatus(null);
            if (rsp.ok) {
                const ev = rsp.data?.events ?? '?';
                vscode.window.showInformationMessage(`xInsp2: recording stopped (${ev} events captured)`);
            } else {
                vscode.window.showWarningMessage(`xInsp2: stop returned ${rsp.error}`);
            }
        }),
        vscode.commands.registerCommand('xinsp2.replayRecording', async () => {
            if (!client?.connected) { vscode.window.showWarningMessage('xInsp2: not connected'); return; }
            const dest = await vscode.window.showOpenDialog({
                canSelectFolders: true, canSelectFiles: false,
                openLabel: 'Replay this recording',
                defaultUri: recordingFolder ? vscode.Uri.file(recordingFolder) : undefined,
            });
            if (!dest || dest.length === 0) return;
            const folder = dest[0].fsPath;
            // Speed picker: real-time / 2x / 0.5x / instant.
            const speedPick = await vscode.window.showQuickPick(
                [
                    { label: '1.0× — real time',     speed: 1.0 },
                    { label: '2.0× — fast',          speed: 2.0 },
                    { label: '0.5× — slow',          speed: 0.5 },
                    { label: '0× — instant (no waits)', speed: 0 },
                ],
                { placeHolder: 'Replay speed' });
            if (!speedPick) return;
            const rsp = await sendCmd('recording_replay', { folder, speed: (speedPick as any).speed });
            if (rsp.ok) {
                vscode.window.showInformationMessage(`xInsp2: replay started @ ${(speedPick as any).speed}× — ${folder}`);
            } else {
                vscode.window.showErrorMessage(`xInsp2: replay failed — ${rsp.error}`);
            }
        }),
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
                setCurrentProject(folder, name);
                lastProjectFolder = folder;          // for auto-respawn replay
                recomputeAutoRespawn();
                updateProjectStatus();
                treeProvider.setProjectOpen(true);
                openScriptIfExists(folder);
                writeCppProperties(folder);   // IDE IntelliSense config (was the backend's job)
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
                setCurrentProject(folder, n);
                lastProjectFolder = folder;          // for auto-respawn replay
                recomputeAutoRespawn();
                updateProjectStatus();
                setCtx('hasInstances', (rsp.data?.instances?.length ?? 0) > 0);
                sendCmd('list_instances');
                treeProvider.setProjectOpen(true);
                openScriptIfExists(folder);
                writeCppProperties(folder);   // IDE IntelliSense config (was the backend's job)
            }
            return rsp;
        })
    );

    // Open the project's inspection.cpp in an editor. Used by the auto-open
    // hooks above and the explicit xinsp2.openScript command.
    function openScriptIfExists(folder: string) {
        const fs = require('fs');
        const candidate = resolveScriptPath(folder);
        if (fs.existsSync(candidate)) {
            vscode.workspace.openTextDocument(candidate).then(doc => {
                vscode.window.showTextDocument(doc, { preserveFocus: true, preview: false });
            }, () => { /* swallow */ });
        }
    }

    // Write <project>/.vscode/c_cpp_properties.json so the C/C++ extension resolves
    // xi/* + OpenCV (inspect.cpp / plugin .cpp are compiled by the backend, not
    // CMake, so the editor can't infer the include set). This moved OUT of the
    // backend core: we read the resolved compile paths via cmd:toolchain_health and
    // mirror them here. Best-effort; never blocks the open. Never clobbers a config
    // the user took ownership of (one without our _generated_by stamp).
    async function writeCppProperties(projDir: string) {
        try {
            const fs = require('fs');
            const h = await sendCmd('toolchain_health');
            const comps: any[] = h?.data?.components || [];
            const byKey: Record<string, string> = {};
            for (const c of comps) if (c?.path) byKey[c.key] = String(c.path).replace(/\\/g, '/');
            const inc = byKey['include'];
            if (!inc) return;   // nothing useful to point at

            const cfgDir  = path.join(projDir, '.vscode');
            const cfgPath = path.join(cfgDir, 'c_cpp_properties.json');
            try {
                const existing = fs.readFileSync(cfgPath, 'utf8');
                if (!existing.includes('"_generated_by": "xinsp2"')) {
                    output.appendLine('[xinsp2] c_cpp_properties.json is user-owned; leaving it');
                    return;
                }
            } catch { /* missing -> write it */ }

            const includePath: string[] = [inc, path.posix.join(path.posix.dirname(inc), 'vendor')];
            if (byKey['opencv'])    includePath.push(byKey['opencv'] + '/include');
            if (byKey['turbojpeg']) includePath.push(byKey['turbojpeg'] + '/include');
            if (byKey['ipp'])       includePath.push(byKey['ipp'] + '/include');
            includePath.push('${workspaceFolder}/**');

            const defines = ['_WIN32', '_WIN64'];
            if (byKey['turbojpeg']) defines.push('XINSP2_HAS_TURBOJPEG=1');

            const cfg = {
                _generated_by: 'xinsp2',
                _note: 'Auto-generated by the xInsp2 VS Code extension on open. Mirrors the '
                     + 'backend compile include set so IntelliSense resolves xi/* and OpenCV. '
                     + 'Delete _generated_by to take manual ownership.',
                version: 4,
                configurations: [{
                    name: 'xInsp2',
                    includePath,
                    forcedInclude: [inc + '/xi/xi_script_support.hpp'],
                    defines,
                    cStandard: 'c17',
                    cppStandard: 'c++20',
                    intelliSenseMode: 'windows-msvc-x64',
                }],
            };
            fs.mkdirSync(cfgDir, { recursive: true });
            fs.writeFileSync(cfgPath, JSON.stringify(cfg, null, 2) + '\n', 'utf8');
            output.appendLine('[xinsp2] wrote IntelliSense config -> ' + cfgPath);

            const extJson = path.join(cfgDir, 'extensions.json');
            if (!fs.existsSync(extJson)) {
                fs.writeFileSync(extJson,
                    JSON.stringify({ recommendations: ['ms-vscode.cpptools'] }, null, 2) + '\n', 'utf8');
            }
        } catch { /* best-effort */ }
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

    // ---- Project Settings webview -------------------------------------
    // One-stop form for everything that lives in project.json. Save
    // writes the file directly + applies live where the backend cares
    // (trigger policy, watchdog). Open via Instances view title bar
    // gear icon, or "xInsp2: Project Settings…" in Command Palette.
    let settingsPanel: vscode.WebviewPanel | undefined;
    context.subscriptions.push(
        vscode.commands.registerCommand('xinsp2.openProjectSettings', async () => {
            if (!currentProjectPath) {
                vscode.window.showWarningMessage('xInsp2: open a project first');
                return;
            }
            const projDir = currentProjectPath;
            const projFile = path.join(projDir, 'project.json');
            const fs = require('fs');
            let pj: any = {};
            try { pj = JSON.parse(fs.readFileSync(projFile, 'utf8')); } catch { pj = {}; }

            // Get available source instance names (for trigger policy fields).
            const instRsp = await sendCmd('list_instances');
            const sources: string[] = (instRsp?.data?.instances || []).map((i: any) => i.name);

            // Read each instance's current group from its instance.json (for the
            // Source→group map in the Parallelism section).
            const instanceGroups: Record<string, string> = {};
            for (const n of sources) {
                try {
                    const ij = path.join(projDir, 'instances', n, 'instance.json');
                    if (fs.existsSync(ij)) {
                        const j = JSON.parse(fs.readFileSync(ij, 'utf8'));
                        if (typeof j.group === 'string' && j.group) instanceGroups[n] = j.group;
                    }
                } catch { /* ignore */ }
            }

            if (settingsPanel) {
                settingsPanel.reveal(vscode.ViewColumn.Active);
            } else {
                settingsPanel = vscode.window.createWebviewPanel(
                    'xinsp2.projectSettings',
                    `Project Settings · ${pj.name || path.basename(projDir)}`,
                    vscode.ViewColumn.Active,
                    { enableScripts: true, retainContextWhenHidden: true }
                );
                settingsPanel.onDidDispose(() => { settingsPanel = undefined; });
                settingsPanel.webview.onDidReceiveMessage(async (msg: any) => {
                    // ---- C++ toolchain health round-trips ----
                    if (msg.type === 'tc_refresh') {
                        const h = await sendCmd('toolchain_health');
                        settingsPanel?.webview.postMessage({ type: 'tc_health', data: h?.data });
                        return;
                    }
                    if (msg.type === 'tc_set' || msg.type === 'tc_clear') {
                        let newPath = '';
                        if (msg.type === 'tc_set') {
                            const isFile = !!msg.isFile;   // vcvars64.bat is a file; the rest are folders
                            const uris = await vscode.window.showOpenDialog({
                                canSelectFolders: !isFile, canSelectFiles: isFile, canSelectMany: false,
                                openLabel: isFile ? 'Use this vcvars64.bat' : 'Use this folder',
                                title: `Pick the ${msg.key} ${isFile ? 'batch file' : 'install folder'}`,
                            });
                            if (!uris || !uris.length) return;   // cancelled
                            newPath = uris[0].fsPath;
                        }
                        const r = await sendCmd('set_toolchain_override', { key: msg.key, path: newPath });
                        if (!r?.ok) {
                            vscode.window.showErrorMessage(`xInsp2: ${r?.error || 'failed to set toolchain path'}`);
                            return;
                        }
                        settingsPanel?.webview.postMessage({ type: 'tc_health', data: r.data?.health });
                        // The change applies on the next compile — offer it now.
                        vscode.window.showInformationMessage(
                            `xInsp2: ${msg.key} path ${msg.type === 'tc_clear' ? 'cleared' : 'updated'}. Recompile to apply.`,
                            'Recompile now',
                        ).then(pick => { if (pick === 'Recompile now') vscode.commands.executeCommand('xinsp2.compile'); });
                        return;
                    }
                    // ---- runtime knobs apply live ----
                    if (msg.type === 'rt_priority') {
                        if (msg.value) {
                            const r = await sendCmd('set_process_priority', { class: msg.value });
                            if (r?.ok) vscode.window.setStatusBarMessage(`xInsp2: process priority → ${msg.value}`, 2500);
                            else vscode.window.showErrorMessage(`xInsp2: ${r?.error || 'bad priority class'}`);
                        }
                        return;
                    }
                    if (msg.type === 'rt_timer_fps') {
                        const r = await sendCmd('set_timer_fps', { fps: msg.value });
                        if (r?.ok) vscode.window.setStatusBarMessage(
                            `xInsp2: timer → ${msg.value <= 0 ? 'trigger-only' : msg.value + ' fps'}`, 2500);
                        return;
                    }
                    if (msg.type !== 'save') return;
                    const next = msg.data || {};
                    // Merge into existing pj — don't drop unknown fields the
                    // user may have hand-edited.
                    let cur: any = {};
                    try { cur = JSON.parse(fs.readFileSync(projFile, 'utf8')); } catch { cur = {}; }
                    cur.name           = next.name ?? cur.name;
                    cur.script         = next.script ?? cur.script;
                    cur.auto_respawn   = next.auto_respawn;
                    if (next.watchdog_ms !== undefined) cur.watchdog_ms = next.watchdog_ms;
                    // Dispatch groups: merge into parallelism (keep dispatch_threads
                    // etc.). No groups → drop the groups/default_group keys (legacy).
                    if (next.parallelism && Array.isArray(next.parallelism.groups) && next.parallelism.groups.length) {
                        cur.parallelism = { ...(cur.parallelism || {}),
                            default_group: next.parallelism.default_group || '',
                            groups: next.parallelism.groups };
                    } else if (cur.parallelism) {
                        delete cur.parallelism.groups; delete cur.parallelism.default_group;
                        if (Object.keys(cur.parallelism).length === 0) delete cur.parallelism;
                    }
                    // Runtime knobs (apply-live; persist here too).
                    if (next.runtime) {
                        const rt: any = { ...(cur.runtime || {}) };
                        if (next.runtime.process_priority) rt.process_priority = next.runtime.process_priority;
                        else delete rt.process_priority;
                        if (typeof next.runtime.timer_fps === 'number' && next.runtime.timer_fps >= 0) rt.timer_fps = next.runtime.timer_fps;
                        else delete rt.timer_fps;
                        if (Object.keys(rt).length) cur.runtime = rt; else delete cur.runtime;
                    }
                    fs.writeFileSync(projFile, JSON.stringify(cur, null, 2) + '\n', 'utf8');
                    output.appendLine('[xinsp2] project.json saved');
                    // Source→group: write each instance.json's `group` field.
                    const ig: Record<string, string> = next.instance_groups || {};
                    for (const n of sources) {
                        try {
                            const ij = path.join(projDir, 'instances', n, 'instance.json');
                            if (!fs.existsSync(ij)) continue;
                            const j = JSON.parse(fs.readFileSync(ij, 'utf8'));
                            const g = ig[n] || '';
                            if (g) j.group = g; else delete j.group;
                            fs.writeFileSync(ij, JSON.stringify(j, null, 2) + '\n', 'utf8');
                        } catch (e) { output.appendLine(`[xinsp2] instance group write failed for ${n}: ${e}`); }
                    }
                    // Apply live where it matters.
                    recomputeAutoRespawn();
                    if (typeof next.watchdog_ms === 'number') {
                        await sendCmd('set_watchdog_ms', { ms: next.watchdog_ms });
                    }
                    vscode.window.setStatusBarMessage('xInsp2: project settings saved', 3000);
                    settingsPanel?.webview.postMessage({ type: 'saved' });
                });
            }
            // (Re)render with current state.
            const par = pj.parallelism || {};
            const state = {
                name:         pj.name || path.basename(projDir),
                script:       pj.script || DEFAULT_SCRIPT_NAME,
                folder:       projDir,
                auto_respawn: pj.auto_respawn !== false,    // default true
                watchdog_ms:  typeof pj.watchdog_ms === 'number' ? pj.watchdog_ms : 0,
                parallelism: {
                    default_group: typeof par.default_group === 'string' ? par.default_group : '',
                    groups:        Array.isArray(par.groups) ? par.groups : [],
                },
                instance_groups: instanceGroups,
                runtime: {
                    process_priority: typeof pj.runtime?.process_priority === 'string' ? pj.runtime.process_priority : '',
                    timer_fps:        typeof pj.runtime?.timer_fps === 'number' ? pj.runtime.timer_fps : -1,
                },
                sources,
            };
            settingsPanel.webview.html = renderProjectSettingsHtml(state);
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
                setCurrentProject(undefined, undefined);
                lastProjectFolder = null;            // user closed; don't replay on respawn
                updateProjectStatus();
                treeProvider.update([], []);
                treeProvider.setProjectOpen(false);
                vscode.window.setStatusBarMessage('xInsp2: project closed', 2000);
            }
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
            // Seed a working script that uses whichever instances we created.
            const scriptPath = path.join(sampleDir, DEFAULT_SCRIPT_NAME);
            const hasCam = plugins.some(x => x.name === 'mock_camera');
            const hasDet = plugins.some(x => x.name === 'blob_analysis');
            if (hasCam && hasDet) {
                fs.writeFileSync(scriptPath, `//
// xInsp2 sample — mock_camera → blob_analysis pipeline.
// Edit, save (compiles automatically), and click Run Inspection.
//
#include <xi/xi.hpp>          // pulls in OpenCV
#include <xi/xi_use.hpp>

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

    // Source is already 1-channel here; if your camera produces RGB,
    // run cv::cvtColor(img.as_cv_mat(), ..., cv::COLOR_RGB2GRAY).
    auto out = det.process(xi::Record().image("gray", img));
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
            if (attachMode) {
                // The extension doesn't own this backend — the xinsp-fe
                // supervisor does. Reconnect, but don't spawn/kill.
                vscode.window.showInformationMessage(
                    'This backend is managed by the xinsp-fe supervisor — restart it there. '
                    + 'Reconnecting…');
                output.appendLine('[xinsp2] attach mode: restart requested — reconnecting only (FE owns the process)');
                setTimeout(() => client!.connect(), 200);
                return;
            }
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
            pluginRegistry.update(r.data as PluginInfo[]);
            setCtx('hasPlugins', r.data.length > 0);
            lastPluginCount = r.data.length;
            setViewBadge(lastConnected, lastInstanceCount, lastPluginCount);
            refreshPluginBrowser();
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
            pluginRegistry.setRemovableFolders(next);
            if (client?.connected) await sendCmd('rescan_plugins', { dir: folder });
            await refreshPlugins();
            vscode.window.showInformationMessage(`xInsp2: added ${folder}`);
        })
    );

    context.subscriptions.push(
        // Remove one of the global xinsp2.extraPluginDirs search roots. (The
        // project-local plugin_dirs roots are managed in the Plugin Browser; this
        // is the workspace-setting side, picked from the palette.)
        vscode.commands.registerCommand('xinsp2.removePluginFolder', async () => {
            const current = config.get<string[]>('extraPluginDirs', []);
            if (!current.length) {
                vscode.window.showInformationMessage('xInsp2: no extra plugin folders configured.');
                return;
            }
            const folder = await vscode.window.showQuickPick(current, { placeHolder: 'Remove which plugin folder?' });
            if (!folder) return;
            const next = current.filter(d => path.resolve(d).toLowerCase() !== path.resolve(folder).toLowerCase());
            await config.update('extraPluginDirs', next, vscode.ConfigurationTarget.Global);
            pluginRegistry.setRemovableFolders(next);
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
                const pickItems: { label: string; description?: string }[] =
                    plugins.data.map((p: any) => ({ label: p.name, description: p.description }));
                const pick = await vscode.window.showQuickPick(pickItems,
                    { placeHolder: 'Select plugin type' });
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
            // Ctrl/⌘+click from the script passes only the instance name — resolve
            // its plugin from the live instance map.
            if (instanceName && !pluginName) pluginName = instanceMap.get(instanceName);
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
        // The open project's resolved script path (from project.json's `script`,
        // not a hardcoded filename) + whether the active editor IS that script.
        // Backs the editor-title Compile/Run gating (xinsp2.isActiveScript).
        get scriptPath() { return currentScriptPath; },
        get activeIsScript() {
            const active = vscode.window.activeTextEditor?.document.uri.fsPath;
            return !!active && !!currentScriptPath &&
                path.resolve(active).toLowerCase() === path.resolve(currentScriptPath).toLowerCase();
        },
        // Drives both the editor Compile/Run gate and the save→recompile trigger:
        // is this path the script or one of its #included project files (and not a
        // plugin source)? Path-based — does not stat the file.
        isProjectScriptFile: (p: string) => isProjectScriptFile(p),
        // Pipeline graph (stage 1) node list — script instances in source order.
        extractPipelineNodes: () => extractPipelineNodes(),
        // Ordered nodes + VAR chips (the script glue between plugin stages).
        extractPipelineItems: () => extractPipelineItems(),
        revealVarSite: (name: string) => revealVarSite(name),
        viewerVarCount: () => viewerProvider.lastVarCount(),
        viewerResolved: () => viewerProvider.isResolved(),
        // Pipeline graph (stage 2) — run once with capture, return reconstructed edges.
        captureGraphEdges: (framePath?: string) => captureGraphEdges(framePath),
        captureAndRenderGraph: () => captureAndRenderGraph(),
        firstProjectFrame: () => firstProjectFrame(),
        renderPipelineGraphHtml: (items: PipelineItem[], edges?: GraphEdge[]) =>
            renderPipelineGraphHtml(items, edges),
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
    // Recompiles the SCRIPT when its source side changes. The script is one TU,
    // so a saved lane file / helper header (#included into inspect.cpp) rebuilds
    // the whole script — we compile currentScriptPath, not the saved file (a
    // header isn't independently compilable). Plugin sources are handled by the
    // separate recompile_project_plugin watcher below and skipped here.
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument(async (doc) => {
            if (!client?.connected) return;
            if (!isProjectScriptFile(doc.fileName)) return;
            // Compile the script itself — whether the user saved inspect.cpp or a
            // file it pulls in.
            const compilePath = currentScriptPath || doc.fileName;
            output.appendLine(`[xinsp2] auto-compile: ${doc.fileName} → script ${compilePath}`);
            try {
                const rsp = await sendCmd('compile_and_load', { path: compilePath });
                // Diagnostics carry their own file (cl.exe reports the header for
                // an error in an #included lane file); fall back to the script.
                applyDiagnostics(rsp.data?.diagnostics, compilePath);
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

    // --- Auto-recompile project plugins on save ---
    // When user edits a .cpp/.hpp under <project>/plugins/<name>/, we
    // call cmd:recompile_project_plugin so the change picks up live.
    // The plugin name is the immediate child folder of <project>/plugins/.
    // Debounce per plugin so a multi-file save doesn't fire N rebuilds.
    const recompileTimers = new Map<string, NodeJS.Timeout>();
    function pluginNameForFile(filePath: string): string | null {
        if (!lastProjectFolder) return null;
        const projPlugins = path.join(lastProjectFolder, 'plugins') + path.sep;
        const norm = path.normalize(filePath);
        if (!norm.toLowerCase().startsWith(projPlugins.toLowerCase())) return null;
        const rest = norm.slice(projPlugins.length);
        const sep = rest.indexOf(path.sep);
        return sep > 0 ? rest.slice(0, sep) : null;
    }
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument((doc) => {
            if (!client?.connected) return;
            const ext = path.extname(doc.fileName).toLowerCase();
            if (!['.cpp', '.cc', '.cxx', '.hpp', '.h'].includes(ext)) return;
            const pname = pluginNameForFile(doc.fileName);
            if (!pname) return;
            // Debounce: cancel any pending rebuild for this plugin and
            // re-arm. 250ms is enough to coalesce multi-file saves.
            const prev = recompileTimers.get(pname);
            if (prev) clearTimeout(prev);
            recompileTimers.set(pname, setTimeout(async () => {
                recompileTimers.delete(pname);
                output.appendLine(`[xinsp2] hot-reload project plugin '${pname}'...`);
                try {
                    const rsp = await sendCmd('recompile_project_plugin', { plugin: pname });
                    applyDiagnostics(rsp.data?.diagnostics, doc.fileName);
                    if (rsp.ok) {
                        const reattached = (rsp.data?.reattached || []).length;
                        output.appendLine(
                            `[xinsp2] plugin '${pname}' rebuilt (${reattached} instance${reattached === 1 ? '' : 's'} re-attached)`);
                        vscode.window.setStatusBarMessage(
                            `xInsp2: ${pname} rebuilt`, 3000);
                    } else {
                        output.appendLine(`[xinsp2] plugin '${pname}' rebuild FAILED: ${rsp.error}`);
                        vscode.window.showErrorMessage(
                            `xInsp2: plugin '${pname}' rebuild failed — check Problems panel`);
                    }
                } catch (e: any) {
                    output.appendLine(`[xinsp2] hot-reload error: ${e.message}`);
                }
            }, 250));
        })
    );

    let g_continuous = false;
    client.on('json', (msg: any) => {
        if (msg.type === 'rsp' && msg.data?.started) g_continuous = true;
        if (msg.type === 'rsp' && msg.data?.stopped)  g_continuous = false;
    });

    // --- Start backend ---

    // Resolve attach-vs-managed, then connect/spawn. Wrapped in an async IIFE
    // because backendMode:"auto" needs a port probe; the rest of activate()
    // (cleanup, return) runs synchronously below.
    void (async () => {
    if (isRemote) {
        // Remote mode: never spawn locally; just connect. autoStart is
        // ignored (docs note this).
        output.appendLine(`[xinsp2] connecting to remote ${wsUrl}`);
        client!.connect();
        return;
    }
    // Resolve managed vs attach (probe the port only when 'auto' needs it).
    const probedOpen = backendMode === 'auto' ? await isPortOpen(port) : false;
    attachMode = resolveBackendMode(backendMode, probedOpen) === 'attach';
    if (backendMode === 'auto') {
        output.appendLine(`[xinsp2] backendMode=auto → ${attachMode
            ? 'attach (a backend is already on the port)' : 'managed (no backend found)'}`);
    }
    if (attachMode) {
        // A supervisor (xinsp-fe.exe) owns the backend. Connect read/operator-
        // only; never spawn or respawn — that's the FE's job.
        output.appendLine(`[xinsp2] attach mode: connecting to supervisor-managed backend at ${wsUrl}`);
        updateHealthStatus(false);
        client!.connect();
        return;
    }
    // Don't spawn a backend for a folder whose project.json isn't ours (the
    // extension may have activated via workspaceContains:project.json). Still
    // spawn for a real xInsp2 project, or an empty folder (create-new via the view).
    if (autoStart && (looksLikeXinspProject || !unrelatedProjectJson)) {
        // Managed mode: bump to the next free port from the configured base so
        // multiple projects each spawn their own backend without colliding. Point
        // the client at it before we spawn + connect, and reflect it in the UI.
        port = await findFreePort(port);
        client!.setUrl(`ws://127.0.0.1:${port}`);
        backendPortInUse = port;
        updateHealthStatus(false);
        output.appendLine(`[xinsp2] managed: backend on free port ${port}`);
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
    })();

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
