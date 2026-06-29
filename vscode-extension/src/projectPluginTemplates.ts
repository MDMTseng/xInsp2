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
        description: 'Background worker thread that emits trigger records',
        detail: 'A simulated camera. Shows xi::spawn_worker for safe threading, host->emit_record for pushing frames into the dispatch pipeline, persistent state across DLL reloads, and exchange() as a control channel (start/stop).',
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

// ----- "From example" choices (sdk/examples/*) -------------------------
//
// The New-Plugin picker offers the 3 tiers above AND every plugin under
// sdk/examples/. Picking an example COPIES it into the project (rename-on-
// copy): the class name, plugin.json name/dll, and .cpp filename all become
// the user's new plugin name. The standalone CMakeLists is dropped — an
// in-project source plugin is compiled by the backend (compile:true).

export interface ExampleChoice {
    dir: string;          // absolute path to sdk/examples/<name>
    name: string;         // example's own plugin name (from plugin.json)
    description: string;
}

export function listExamplePlugins(sdkRoot: string): ExampleChoice[] {
    const exRoot = path.join(sdkRoot, 'examples');
    let entries: string[] = [];
    try { entries = fs.readdirSync(exRoot); } catch { return []; }
    const out: ExampleChoice[] = [];
    for (const e of entries.sort()) {
        const dir = path.join(exRoot, e);
        const pj = path.join(dir, 'plugin.json');
        try {
            if (!fs.statSync(dir).isDirectory() || !fs.existsSync(pj)) continue;
            const m = JSON.parse(fs.readFileSync(pj, 'utf8'));
            if (!m.name) continue;
            out.push({ dir, name: m.name, description: m.description || '' });
        } catch { /* skip unreadable example */ }
    }
    return out;
}

// my_filter / my-filter → MyFilter (a C++ class identifier).
function toPascalClass(name: string): string {
    const parts = name.split(/[^A-Za-z0-9]+/).filter(Boolean);
    const p = parts.map(s => s.charAt(0).toUpperCase() + s.slice(1)).join('');
    return /^[A-Za-z]/.test(p) ? p : 'Plugin' + p;
}

// Read sdk/examples/<dir> and rewrite it as a new plugin named `newName`.
// Returns the same {relPath -> content} shape as renderPluginFiles, with the
// .cpp at the folder root (the source-plugin convention) — NOT under src/.
export function renderExamplePluginFiles(
    exampleDir: string,
    newName: string,
    description: string,
): { files: Map<string, string>; cppRel: string } {
    const files = new Map<string, string>();

    // Find the example's single .cpp (the plugin source).
    const cpps = fs.readdirSync(exampleDir).filter(f => f.endsWith('.cpp'));
    if (cpps.length === 0) throw new Error(`example ${exampleDir} has no .cpp`);
    const srcRel = cpps[0];
    let src = fs.readFileSync(path.join(exampleDir, srcRel), 'utf8');

    // Rename the C++ class: XI_PLUGIN_IMPL(<Class>) is the reliable anchor.
    const m = src.match(/XI_PLUGIN_IMPL\s*\(\s*([A-Za-z_]\w*)\s*\)/);
    if (m) {
        const oldClass = m[1];
        const newClass = toPascalClass(newName);
        if (oldClass !== newClass)
            src = src.replace(new RegExp(`\\b${oldClass}\\b`, 'g'), newClass);
    }
    files.set(`${newName}.cpp`, src);

    // plugin.json: keep every flag (sink, reentrant, manifest, …) but rename.
    let pj: any = {};
    try { pj = JSON.parse(fs.readFileSync(path.join(exampleDir, 'plugin.json'), 'utf8')); } catch { pj = {}; }
    pj.name = newName;
    pj.dll  = `${newName}.dll`;
    if (description) pj.description = description;
    files.set('plugin.json', JSON.stringify(pj, null, 2) + '\n');

    return { files, cppRel: `${newName}.cpp` };
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
