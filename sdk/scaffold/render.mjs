// render.mjs — single source-of-truth template renderer for xInsp2
// plugin scaffolding. Both the VS Code extension's "New Project Plugin"
// command AND the SDK CLI (sdk/scaffold.mjs) call into this module so
// neither maintains its own copy of the templates.
//
// Templates live at sdk/templates/{easy,medium,expert}/ as ordinary
// files (plugin.cpp, plugin.json.tpl, ui/index.html.tpl, etc.). We
// substitute {{NAME}} / {{CLASS}} / {{DESCRIPTION}} / {{TEMPLATE_ID}}
// inline; that's enough for the current set of placeholders. No mustache
// dependency — keeps the SDK shippable as plain Node scripts with zero
// install step.
//
// The `_shared/` subtree under sdk/templates/ holds reusable snippets
// (e.g. the inline image-viewer-with-pan-zoom widget) that some
// templates pull in via {{INCLUDE _shared/<path>}}.
//
// Output shape:
//
//   const files = renderTemplate('medium', {
//       NAME: 'my_filter',
//       CLASS: 'MyFilter',
//       DESCRIPTION: 'threshold demo',
//   }, { mode: 'in-project' });
//
//   // files is a Map<relativePath, contentString>:
//   //   'plugin.json'        → '{ ... }'
//   //   'src/plugin.cpp'     → '// my_filter ...'
//   //   'ui/index.html'      → '<!doctype ...'
//
// `mode` controls which files are emitted:
//   'in-project'  — used by the VS Code extension. Backend compiles
//                   the plugin in place, so we DON'T emit CMakeLists or
//                   tests/. Just the .cpp + plugin.json + optional ui/.
//   'standalone'  — used by the SDK CLI. Adds CMakeLists.txt + tests/
//                   so the plugin can build outside the backend tree.

import { readFile, readdir, stat } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, relative, resolve } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));
// sdk/templates is one level up from this file (sdk/scaffold/render.mjs).
const TEMPLATES_ROOT = resolve(__dirname, '..', 'templates');

export const TEMPLATE_IDS = ['easy', 'medium', 'expert'];

/**
 * Render a template into a Map<relPath, content>. Returns the in-memory
 * file set; the caller decides where to write them.
 *
 * @param {string} templateId one of 'easy' | 'medium' | 'expert'
 * @param {Record<string, string>} vars    { NAME, CLASS, DESCRIPTION, ... }
 * @param {{ mode?: 'in-project' | 'standalone' }} options
 * @returns {Promise<Map<string, string>>}
 */
export async function renderTemplate(templateId, vars, options = {}) {
    const mode = options.mode || 'in-project';
    if (!TEMPLATE_IDS.includes(templateId)) {
        throw new Error(`unknown template id: ${templateId}`);
    }
    const tplDir = join(TEMPLATES_ROOT, templateId);
    const filledVars = withDefaults(vars, templateId);

    // Walk the template directory; for each file decide:
    //   - skip if the file's mode tag (.in-project / .standalone) doesn't match
    //   - otherwise read, expand placeholders + includes, store in result
    const files = new Map();
    for (const f of await walk(tplDir)) {
        const relPath = relative(tplDir, f);
        const out = decideOutPath(relPath, mode);
        if (!out) continue;
        const raw = await readFile(f, 'utf8');
        const expanded = await render(raw, filledVars);
        files.set(out, expanded);
    }
    return files;
}

/** Convenience for the SDK CLI: render then write to disk under destDir. */
export async function renderAndWrite(templateId, vars, destDir, options = {}) {
    const files = await renderTemplate(templateId, vars, options);
    const { mkdir, writeFile } = await import('node:fs/promises');
    for (const [rel, content] of files) {
        const full = join(destDir, rel);
        await mkdir(dirname(full), { recursive: true });
        await writeFile(full, content, 'utf8');
    }
    return files;
}

/** Where the templates live on disk — for the extension to surface. */
export function templatesRoot() { return TEMPLATES_ROOT; }

// ---------------------------------------------------------------------------

async function walk(dir) {
    const out = [];
    for (const ent of await readdir(dir, { withFileTypes: true })) {
        if (ent.name === '_shared') continue; // shared snippets aren't emitted
        const p = join(dir, ent.name);
        if (ent.isDirectory()) out.push(...await walk(p));
        else                   out.push(p);
    }
    return out;
}

// File-mode-gating rules for the standalone vs in-project distinction.
// Files are NEVER renamed at the top level — naming convention is purely
// based on the leaf filename's `.in-project` / `.standalone` suffix
// before the regular extension. A file with no mode tag is emitted in
// both modes. .tpl is stripped from the output filename.
//
//   plugin.cpp                 → both modes, output 'plugin.cpp' (rare)
//   src/plugin.cpp             → both modes, output 'src/plugin.cpp'
//   plugin.json.tpl            → both modes, output 'plugin.json'
//   CMakeLists.txt.standalone  → standalone only, output 'CMakeLists.txt'
//   tests/test.cjs.standalone  → standalone only
//
// This keeps the SAME file set readable from disk while a single mode
// flag selects what to emit.
function decideOutPath(relPath, mode) {
    // Normalise separators so behaviour is identical on win/linux.
    const norm = relPath.split(/[\\/]/).join('/');
    const parts = norm.split('/');
    const leaf = parts[parts.length - 1];
    let outLeaf = leaf;
    let modeTag = null;
    // Detect a `.in-project` / `.standalone` mode tag right before .tpl
    // (or the actual extension). e.g. 'CMakeLists.txt.standalone' →
    // mode='standalone', emitted leaf 'CMakeLists.txt'.
    const m = leaf.match(/^(.*)\.(in-project|standalone)(\.tpl)?$/);
    if (m) {
        modeTag = m[2];
        outLeaf = m[1] + (m[3] ? '' : ''); // drop the tag and any trailing .tpl
    } else if (leaf.endsWith('.tpl')) {
        outLeaf = leaf.slice(0, -4);
    }
    if (modeTag && modeTag !== mode) return null;
    parts[parts.length - 1] = outLeaf;
    return parts.join('/');
}

// Substitute {{KEY}} and expand {{INCLUDE relpath}} references. Includes
// are resolved against TEMPLATES_ROOT (not the calling template's dir),
// so 'INCLUDE _shared/foo.html' always means the same thing.
async function render(text, vars) {
    // Includes first (so substitutions in the included content also run).
    text = await expandIncludes(text);
    // {{KEY}} → vars[KEY]; unknown keys are left as-is to surface bugs.
    return text.replace(/\{\{([A-Z_]+)\}\}/g, (m, key) => {
        return Object.prototype.hasOwnProperty.call(vars, key) ? vars[key] : m;
    });
}

async function expandIncludes(text) {
    // {{INCLUDE relpath}} where relpath is rooted at sdk/templates/.
    const re = /\{\{INCLUDE\s+([A-Za-z0-9_/.\-]+)\s*\}\}/g;
    let out = '', last = 0, m;
    while ((m = re.exec(text)) !== null) {
        out += text.slice(last, m.index);
        const incPath = resolve(TEMPLATES_ROOT, m[1]);
        out += await readFile(incPath, 'utf8');
        last = m.index + m[0].length;
    }
    out += text.slice(last);
    return out;
}

function withDefaults(vars, templateId) {
    const v = { ...vars };
    if (!v.NAME)        throw new Error('renderTemplate: NAME is required');
    if (!v.CLASS)       v.CLASS = toClassName(v.NAME);
    if (!v.DESCRIPTION) v.DESCRIPTION = `${templateId} template plugin: ${v.NAME}`;
    v.TEMPLATE_ID = templateId;
    return v;
}

function toClassName(folder) {
    const parts = String(folder).split(/[^A-Za-z0-9]+/).filter(Boolean);
    if (!parts.length) return 'MyPlugin';
    return parts.map(p => p[0].toUpperCase() + p.slice(1)).join('');
}
