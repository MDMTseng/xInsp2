#!/usr/bin/env node
//
// scaffold.mjs — bootstrap a new xInsp2 plugin in an external folder.
//
// Usage:
//   node <xinsp2>/sdk/scaffold.mjs <output-dir> [--name <name>]
//
// Examples:
//   node sdk/scaffold.mjs C:\me\my_plugins\foo
//   node sdk/scaffold.mjs ../../my_plugins/foo --name foo
//
// Creates <output-dir>/{plugin.json, <name>.cpp, ui/index.html, CMakeLists.txt}
// from sdk/template/, with placeholders replaced by the chosen plugin name.
// Will refuse to overwrite existing files unless --force is passed.
//
// The generated CMakeLists references xInsp2 via the XINSP2_ROOT env var.
// Build with:
//   set XINSP2_ROOT=<path-to-xInsp2>
//   cmake -S <plugin-dir> -B <plugin-dir>/build -A x64
//   cmake --build <plugin-dir>/build --config Release
//

import { fileURLToPath } from 'node:url';
import { dirname, resolve, basename, join } from 'node:path';
import { existsSync, mkdirSync, copyFileSync, readFileSync, writeFileSync, readdirSync, statSync } from 'node:fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const TEMPLATE_DIR = resolve(__dirname, 'template');
const XINSP2_ROOT = resolve(__dirname, '..');

// ---- args --------------------------------------------------------------

const args = process.argv.slice(2);
let outDir = null;
let name = null;
let force = false;
for (let i = 0; i < args.length; ++i) {
    const a = args[i];
    if (a === '--name' && i + 1 < args.length) name = args[++i];
    else if (a === '--force') force = true;
    else if (a === '-h' || a === '--help') {
        console.log(`usage: scaffold.mjs <output-dir> [--name <name>] [--force]`);
        process.exit(0);
    } else if (!a.startsWith('--')) outDir = resolve(a);
    else { console.error(`unknown flag: ${a}`); process.exit(2); }
}
if (!outDir) {
    console.error('error: <output-dir> is required');
    console.error('usage: scaffold.mjs <output-dir> [--name <name>] [--force]');
    process.exit(2);
}
if (!name) name = basename(outDir);
if (!/^[a-z][a-z0-9_]*$/.test(name)) {
    console.error(`error: --name must match [a-z][a-z0-9_]* (got "${name}")`);
    process.exit(2);
}

// ---- copy with substitution -------------------------------------------

mkdirSync(outDir, { recursive: true });

function walk(srcDir, dstDir) {
    for (const entry of readdirSync(srcDir)) {
        const src = join(srcDir, entry);
        const dst = join(dstDir, entry.replace(/my_plugin/g, name));
        const st = statSync(src);
        if (st.isDirectory()) {
            mkdirSync(dst, { recursive: true });
            walk(src, dst);
        } else {
            if (existsSync(dst) && !force) {
                console.error(`refusing to overwrite ${dst} (pass --force)`);
                process.exit(2);
            }
            // Text-substitute in plain-text files. Three tokens:
            //   my_plugin → snake_case        (filenames, plugin name)
            //   MyPlugin  → PascalCase        (C++ class)
            //   MY_PLUGIN → UPPER_SNAKE_CASE  (preprocessor macros)
            const ext = entry.split('.').pop().toLowerCase();
            if (['cpp', 'hpp', 'h', 'json', 'html', 'txt', 'cmake', 'md', 'cjs', 'mjs', 'js'].includes(ext)) {
                const pascal = name.split('_')
                    .map(s => s.charAt(0).toUpperCase() + s.slice(1))
                    .join('');
                const upper = name.toUpperCase();
                const body = readFileSync(src, 'utf8')
                    .replace(/MY_PLUGIN/g, upper)
                    .replace(/MyPlugin/g, pascal)
                    .replace(/my_plugin/g, name);
                writeFileSync(dst, body);
            } else {
                copyFileSync(src, dst);
            }
        }
    }
}

walk(TEMPLATE_DIR, outDir);

// ---- final summary ----------------------------------------------------

console.log(`scaffolded plugin "${name}" → ${outDir}\n`);
console.log(`next steps:`);
console.log(`  set XINSP2_ROOT=${XINSP2_ROOT}`);
console.log(`  cmake -S "${outDir}" -B "${outDir}/build" -A x64`);
console.log(`  cmake --build "${outDir}/build" --config Release`);
console.log(``);
console.log(`then either:`);
console.log(`  - launch xInsp2 with --plugins-dir=${dirname(outDir)}, or`);
console.log(`  - add ${dirname(outDir)} to xinsp2.extraPluginDirs in VS Code settings`);
