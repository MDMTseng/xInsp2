// projectPluginTemplates.ts — thin wrapper over the SDK's shared
// template renderer. The actual templates + render logic live at
// sdk/scaffold/render.mjs (see that file's header for the format).
// This module just locates the SDK on disk + adapts the result to
// the extension's existing call shape.
//
// Why this exists: keeping ONE source of truth for the templates
// shared by the VS Code "New Project Plugin…" command AND the
// `sdk/scaffold.mjs` CLI. Same files render the same plugin from
// either entry point.

import * as path from 'path';
import * as fs from 'fs';

export type TemplateId = 'easy' | 'medium' | 'expert';

export interface TemplateChoice {
    id: TemplateId;
    label: string;
    description: string;
    detail: string;
}

// QuickPick choices stay here so the extension's UI strings live with
// the extension, but template CONTENT lives in sdk/templates/.
export const TEMPLATE_CHOICES: TemplateChoice[] = [
    {
        id: 'easy',
        label: '$(symbol-class)  Easy — pass-through',
        description: 'Minimal plugin shell',
        detail: 'A do-nothing class wired up correctly. Best place to start: shows constructor / def / process / exchange in 30 lines. Edit process() and you have a working plugin.',
    },
    {
        id: 'medium',
        label: '$(symbol-method)  Medium — image processor',
        description: 'Reads input image, applies threshold, emits binary image + blob count',
        detail: 'Demonstrates the Record image API, JSON-driven config (threshold from set_def), and how to publish a result image back to the script.',
    },
    {
        id: 'expert',
        label: '$(symbol-event)  Expert — stateful source',
        description: 'Background worker thread that emits trigger frames into the bus',
        detail: 'A simulated camera. Shows xi::spawn_worker for safe threading, host_api->emit_trigger for pushing into the bus, persistent state across DLL reloads, and exchange() as a control channel (start/stop).',
    },
];

// ----- SDK location resolver -------------------------------------------
//
// Order:
//   1. workspace setting `xinsp2.sdkPath` (explicit override)
//   2. extension's own dev parent: <ext>/.. has `sdk/`?  (workspace dev)
//   3. backend exe's parent: <backend_dir>/../sdk/         (packaged install)
// Throws if none of the above resolves to a directory containing
// `templates/`. Caller surfaces as an error toast.

let cachedSdkRoot: string | null = null;

function tryDir(p: string | undefined): string | null {
    if (!p) return null;
    try {
        const t = path.join(p, 'templates');
        if (fs.existsSync(t) && fs.statSync(t).isDirectory()) return p;
    } catch { /* fall through */ }
    return null;
}

export function locateSdkRoot(extensionPath: string,
                               backendExePath?: string,
                               configuredPath?: string): string {
    if (cachedSdkRoot) return cachedSdkRoot;
    const candidates: (string | null)[] = [
        tryDir(configuredPath),
        // ext at <root>/vscode-extension → SDK at <root>/sdk
        tryDir(path.resolve(extensionPath, '..', 'sdk')),
        // packaged: backend at <install>/backend/.../xinsp-backend.exe
        backendExePath
            ? tryDir(path.resolve(path.dirname(backendExePath), '..', '..', 'sdk'))
            : null,
        // packaged alt: backend at <install>/xinsp-backend.exe → <install>/sdk
        backendExePath
            ? tryDir(path.resolve(path.dirname(backendExePath), '..', 'sdk'))
            : null,
    ];
    for (const c of candidates) {
        if (c) { cachedSdkRoot = c; return c; }
    }
    throw new Error('SDK templates not found. Set xinsp2.sdkPath in settings, '
        + 'or install xInsp2 with the sdk/ folder alongside the extension.');
}

// ----- Render delegate -------------------------------------------------
//
// Imports sdk/scaffold/render.mjs at runtime. Since the bundled
// extension is CommonJS (esbuild's default for VS Code) we use a
// dynamic import — render.mjs is ESM.

export async function renderPluginFiles(
    sdkRoot: string,
    template: TemplateId,
    name: string,
    description: string,
): Promise<Map<string, string>> {
    const renderModulePath = path.join(sdkRoot, 'scaffold', 'render.mjs');
    if (!fs.existsSync(renderModulePath)) {
        throw new Error(`SDK renderer not found at ${renderModulePath}`);
    }
    // Dynamic import — file:// URL on Windows for ESM compat.
    const url = require('url').pathToFileURL(renderModulePath).href;
    const mod = await (Function('u', 'return import(u)') as any)(url);
    const files = await mod.renderTemplate(template, {
        NAME: name,
        DESCRIPTION: description,
    }, { mode: 'in-project' });
    return files as Map<string, string>;
}
