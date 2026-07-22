// exampleProjects.ts — the shipped example catalogue, read from disk.
//
// Every official plugin ships toolbox/<name>/example/, and toolbox/example/ is
// the cross-plugin station. Nothing about that list is hardcoded here: this
// module scans for it, so adding an example to the repo makes it appear in the
// picker with no extension change. (The command it feeds used to be a ~120-line
// generator with an inspect.cpp embedded in a TypeScript template literal —
// which drifted from the real examples the moment either side changed.)
//
// NAMING, deliberately asymmetric: the SOURCE tree calls the folder `toolbox/`,
// but a shipped release lays it down as `plugins/` because the backend
// auto-scans <cwd>/plugins. Both are probed, source name first.
import * as fs from 'fs';
import * as path from 'path';

export interface ExampleProject {
    id: string;        // "cache" | "toolbox" — the run_qa suffix, i.e. example_<id>
    dir: string;       // absolute path to the example project folder
    title: string;     // from the README's first heading
    summary: string;   // the README's first prose line
    isStation: boolean; // the cross-plugin one, which we sort to the top
}

function isExampleDir(d: string): boolean {
    return fs.existsSync(path.join(d, 'project.json'))
        && fs.existsSync(path.join(d, 'inspect.cpp'));
}

function tryDir(p: string | null | undefined): string | null {
    try { return p && fs.statSync(p).isDirectory() ? p : null; } catch { return null; }
}

/** Find the folder holding the shipped plugins (and therefore their examples). */
export function locateExampleRoot(extensionPath: string,
                                  backendExePath?: string,
                                  configuredPath?: string): string | null {
    const near = backendExePath ? path.dirname(backendExePath) : null;
    const candidates = [
        configuredPath,
        // dev tree: ext at <root>/vscode-extension
        path.resolve(extensionPath, '..', 'toolbox'),
        path.resolve(extensionPath, '..', 'plugins'),
        // packaged: backend at <install>/bin/xinsp-backend
        near ? path.resolve(near, '..', 'plugins') : null,
        near ? path.resolve(near, '..', 'toolbox') : null,
        near ? path.resolve(near, 'plugins') : null,
    ];
    for (const c of candidates) { const d = tryDir(c); if (d) return d; }
    return null;
}

/** Pull a human title + one-line summary out of the example's README. */
function readCard(dir: string, id: string): { title: string; summary: string } {
    let title = id, summary = '';
    try {
        const md = fs.readFileSync(path.join(dir, 'README.md'), 'utf8');
        const lines = md.split(/\r?\n/);
        const h = lines.findIndex((l) => l.startsWith('# '));
        if (h >= 0) title = lines[h].slice(2).trim();
        // First prose line after the heading: skip blanks, fences, and markup-only
        // lines so the summary is a sentence rather than a table row.
        for (const l of lines.slice(h + 1)) {
            const t = l.trim();
            if (!t || t.startsWith('#') || t.startsWith('```') || t.startsWith('|')
                || t.startsWith('-') || t.startsWith('>')) continue;
            summary = t.replace(/\*\*/g, '').replace(/`/g, '');
            break;
        }
    } catch { /* a README is optional; the id still identifies it */ }
    return { title, summary };
}

/** Every example under `root`, station first, then plugins alphabetically. */
export function listExampleProjects(root: string): ExampleProject[] {
    const out: ExampleProject[] = [];
    const station = path.join(root, 'example');
    if (isExampleDir(station))
        out.push({ id: 'toolbox', dir: station, isStation: true, ...readCard(station, 'toolbox') });
    let names: string[] = [];
    try { names = fs.readdirSync(root).sort(); } catch { return out; }
    for (const n of names) {
        if (n === 'example' || n === 'build') continue;
        const d = path.join(root, n, 'example');
        if (isExampleDir(d)) out.push({ id: n, dir: d, isStation: false, ...readCard(d, n) });
    }
    return out;
}

// Build output, logs and run artifacts. An example is copied so the user can
// edit it freely; carrying a stale .so or another machine's owner-stamp into
// their copy would make the first run behave differently from a clean one.
const SKIP_DIRS = new Set(['build', 'captures', '.xinsp_work', '__pycache__', 'script_build']);
const SKIP_FILE = /^(backend.*\.log|\.xinsp_owner|.*\.log\.(hb|health))$/;

/** Recursively copy an example into `dest`, leaving build/run artifacts behind. */
export function copyExample(src: string, dest: string): void {
    fs.mkdirSync(dest, { recursive: true });
    for (const e of fs.readdirSync(src, { withFileTypes: true })) {
        if (e.isDirectory()) {
            if (SKIP_DIRS.has(e.name)) continue;
            copyExample(path.join(src, e.name), path.join(dest, e.name));
        } else if (e.isFile()) {
            if (SKIP_FILE.test(e.name)) continue;
            fs.copyFileSync(path.join(src, e.name), path.join(dest, e.name));
        }
    }
}
