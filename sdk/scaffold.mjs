#!/usr/bin/env node
//
// scaffold.mjs — bootstrap a new xInsp2 plugin in an external folder.
//
// Usage:
//   node <xinsp2>/sdk/scaffold.mjs <output-dir>
//          [--name <name>]
//          [--template easy|medium|expert]    (default: easy)
//          [--description "<text>"]
//          [--force]
//
// Examples:
//   node sdk/scaffold.mjs C:\me\my_plugins\foo
//   node sdk/scaffold.mjs ../../my_plugins/foo --name foo --template medium
//
// The actual file content lives at sdk/templates/{easy,medium,expert}/
// — same templates the VS Code "New Project Plugin…" command uses.
// `mode: 'standalone'` is set so the output also includes
// CMakeLists.txt + README.md for self-contained out-of-tree builds.
//

import { fileURLToPath } from 'node:url';
import { dirname, resolve, basename } from 'node:path';
import { existsSync } from 'node:fs';
import { renderAndWrite, TEMPLATE_IDS } from './scaffold/render.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const XINSP2_ROOT = resolve(__dirname, '..');

// ---- args --------------------------------------------------------------

const args = process.argv.slice(2);
let outDir = null;
let name = null;
let template = 'easy';
let description = null;
let force = false;
for (let i = 0; i < args.length; ++i) {
    const a = args[i];
    if (a === '--name' && i + 1 < args.length)        name = args[++i];
    else if (a === '--template' && i + 1 < args.length) template = args[++i];
    else if (a === '--description' && i + 1 < args.length) description = args[++i];
    else if (a === '--force')                         force = true;
    else if (a === '-h' || a === '--help') {
        console.log(`usage: scaffold.mjs <output-dir> [--name <name>] `
            + `[--template easy|medium|expert] [--description <text>] [--force]`);
        process.exit(0);
    } else if (!a.startsWith('--')) outDir = resolve(a);
    else { console.error(`unknown flag: ${a}`); process.exit(2); }
}
if (!outDir) {
    console.error('error: <output-dir> is required');
    console.error('usage: scaffold.mjs <output-dir> [--name <name>] [--template <id>] [--force]');
    process.exit(2);
}
if (!name) name = basename(outDir);
if (!/^[a-z][a-z0-9_]*$/.test(name)) {
    console.error(`error: --name must match [a-z][a-z0-9_]* (got "${name}")`);
    process.exit(2);
}
if (!TEMPLATE_IDS.includes(template)) {
    console.error(`error: --template must be one of ${TEMPLATE_IDS.join(' / ')} (got "${template}")`);
    process.exit(2);
}

if (existsSync(outDir) && !force) {
    // Renderer overwrites unconditionally; we gate that here so a
    // stray re-run of scaffold can't clobber edits.
    const { readdirSync } = await import('node:fs');
    if (readdirSync(outDir).length > 0) {
        console.error(`refusing to scaffold into non-empty ${outDir} (pass --force)`);
        process.exit(2);
    }
}

// ---- render via the SDK's shared renderer (same as VS Code extension) -

await renderAndWrite(template, {
    NAME: name,
    DESCRIPTION: description || `${template} template plugin: ${name}`,
}, outDir, { mode: 'standalone' });

// ---- final summary ----------------------------------------------------

console.log(`scaffolded plugin "${name}" (template: ${template}) → ${outDir}\n`);
console.log(`next steps:`);
console.log(`  set XINSP2_ROOT=${XINSP2_ROOT}`);
console.log(`  cmake -S "${outDir}" -B "${outDir}/build" -A x64`);
console.log(`  cmake --build "${outDir}/build" --config Release`);
console.log(``);
console.log(`then either:`);
console.log(`  - launch xInsp2 with --plugins-dir=${dirname(outDir)}, or`);
console.log(`  - add ${dirname(outDir)} to xinsp2.extraPluginDirs in VS Code settings`);
