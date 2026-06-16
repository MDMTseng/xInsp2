//
// lint_plugin_ui.mjs — lint a plugin's instance UI for the automation conventions
// (docs/guides/write-a-plugin.md). Pure Node, no VS Code — runs in ms.
//
// Checks ui/index.html (when plugin.json has "has_ui": true):
//   - every range/number/checkbox/select control carries data-param="<name>"
//   - every <button> carries data-action="<name>"
// so a generic harness (h.setParam / h.action) can drive the UI by param name.
//
// Findings are WARNINGS by default (don't break existing plugins); pass --strict
// to exit non-zero on any finding (for CI / a future cert gate).
//
// Usage: node lint_plugin_ui.mjs <plugin-folder> [--strict]
//
import { readFileSync, existsSync } from 'node:fs';
import { resolve, join } from 'node:path';

const pluginFolder = resolve(process.argv[2] || process.cwd());
const strict = process.argv.includes('--strict');

function fail(msg) { console.error(msg); process.exit(2); }

const pjPath = join(pluginFolder, 'plugin.json');
if (!existsSync(pjPath)) fail(`no plugin.json at ${pluginFolder}`);
let pj = {};
try { pj = JSON.parse(readFileSync(pjPath, 'utf8')); } catch (e) { fail(`plugin.json parse error: ${e.message}`); }

const htmlPath = join(pluginFolder, 'ui', 'index.html');
if (!pj.has_ui || !existsSync(htmlPath)) {
    console.log(`lint_plugin_ui: ${pj.name || pluginFolder}: no instance UI — nothing to lint.`);
    process.exit(0);
}

const html = readFileSync(htmlPath, 'utf8');
const warnings = [];

// Param controls: range/number/checkbox inputs + selects that edit a value.
const controlRe = /<(input|select)\b[^>]*>/gi;
for (const m of html.matchAll(controlRe)) {
    const tag = m[0];
    // skip input types that aren't parameter controls
    const typeM = /\btype\s*=\s*["']?([a-z]+)/i.exec(tag);
    const type = typeM ? typeM[1].toLowerCase() : (m[1].toLowerCase() === 'select' ? 'select' : 'text');
    if (['button', 'submit', 'hidden', 'file'].includes(type)) continue;
    if (!/\bdata-param\s*=/.test(tag)) {
        const idM = /\bid\s*=\s*["']?([\w-]+)/.exec(tag);
        warnings.push(`control ${idM ? `#${idM[1]}` : `<${m[1]} type=${type}>`} has no data-param="<name>"`);
    }
}
// Buttons: should carry data-action.
for (const m of html.matchAll(/<button\b[^>]*>/gi)) {
    if (!/\bdata-action\s*=/.test(m[0])) {
        const idM = /\bid\s*=\s*["']?([\w-]+)/.exec(m[0]);
        warnings.push(`button ${idM ? `#${idM[1]}` : '(no id)'} has no data-action="<name>"`);
    }
}

const name = pj.name || pluginFolder;
if (warnings.length === 0) {
    console.log(`lint_plugin_ui: ${name}: OK — all controls/buttons carry data-param/data-action.`);
    process.exit(0);
}
console.error(`lint_plugin_ui: ${name}: ${warnings.length} convention warning(s) ` +
              `(see docs/guides/write-a-plugin.md):`);
for (const w of warnings) console.error(`  - ${w}`);
process.exit(strict ? 1 : 0);
