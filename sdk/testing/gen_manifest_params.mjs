#!/usr/bin/env node
// gen_manifest_params.mjs — seed (or check) a plugin's manifest.params from the
// keys its get_def() actually emits, via the xi_run_plugin host-mock.
//
// The throughline of DM-7/8/10: a plugin's tunables live in several places with
// no single source of truth, so they drift silently. get_def() is the runtime
// truth for what a plugin persists; this tool reads it and either seeds a
// manifest.params skeleton (DM-10) or asserts manifest.params agrees with it
// (DM-7 consistency gate).
//
// Usage:
//   node gen_manifest_params.mjs <plugin.dll> [--def <json>] [--runner <exe>]
//        seed: prints a manifest.params skeleton (scalar get_def keys; fill range/doc)
//   node gen_manifest_params.mjs <plugin.dll> --check <plugin.json> [--def ...]
//        check: exit 1 if the scalar get_def keys != manifest.params names (drift)
//
// Only SCALARS are tunables (matches the validator, DM-8): arrays/objects in
// get_def are persisted structured state, not manifest.params.

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));

function die(msg) { console.error('gen_manifest_params: ' + msg); process.exit(2); }

// --- args ---
const argv = process.argv.slice(2);
if (!argv.length || argv[0].startsWith('--')) {
    die('usage: gen_manifest_params <plugin.dll> [--def <json>] [--check <plugin.json>] [--runner <exe>]');
}
const dll = argv[0];
let def = null, checkPath = null, runner = null;
for (let i = 1; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--def') def = argv[++i];
    else if (a === '--check') checkPath = argv[++i];
    else if (a === '--runner') runner = argv[++i];
    else die('unknown arg: ' + a);
}
if (!existsSync(dll)) die('plugin DLL not found: ' + dll);

// --- locate xi_run_plugin.exe ---
if (!runner) {
    const root = process.env.XINSP2_ROOT || resolve(__dirname, '..', '..');
    runner = join(root, 'sdk', 'host_mock', 'xi_run_plugin.exe');
}
if (!existsSync(runner)) {
    die('xi_run_plugin not found at ' + runner + '\n  build it once:\n' +
        '    cmake -S %XINSP2_ROOT%\\sdk\\host_mock -B %XINSP2_ROOT%\\sdk\\host_mock\\build -A x64 -DXINSP2_ROOT=%XINSP2_ROOT%\n' +
        '    cmake --build %XINSP2_ROOT%\\sdk\\host_mock\\build --config Release\n' +
        '  or pass --runner <path-to-xi_run_plugin.exe>');
}

// --- ensure OpenCV DLLs are reachable (xi_run_plugin links OpenCV) ---
const ocvCandidates = [
    'C:/opencv/opencv/build/x64/vc16/bin', 'C:/opencv/opencv/build/x64/vc17/bin',
    'C:/opencv/build/x64/vc16/bin', 'C:/opencv/build/x64/vc17/bin',
];
const env = { ...process.env };
for (const d of ocvCandidates) if (existsSync(d)) { env.PATH = d + ';' + (env.PATH || ''); break; }

// --- run get_def via the host-mock ---
const args = [dll, '--dump-def'];
if (def) args.push('--def', def);
let out;
try {
    out = execFileSync(runner, args, { env, encoding: 'utf8' });
} catch (e) {
    die('xi_run_plugin --dump-def failed: ' + (e.stderr || e.message));
}
let def_obj;
try { def_obj = JSON.parse(out.trim()); } catch { die('get_def() did not return JSON:\n' + out); }

const inferType = (v) => Number.isInteger(v) ? 'int'
    : typeof v === 'number' ? 'float'
    : typeof v === 'boolean' ? 'bool'
    : typeof v === 'string' ? 'string' : null;

// scalar keys only (skip arrays/objects = persisted structured state, DM-8)
const scalarKeys = Object.keys(def_obj).filter((k) => {
    const v = def_obj[k];
    return v === null ? false : (typeof v !== 'object');
});

if (!checkPath) {
    // --- seed mode: print a manifest.params skeleton ---
    const params = scalarKeys.map((k) => ({ name: k, type: inferType(def_obj[k]) || 'float', default: def_obj[k] }));
    console.log(JSON.stringify({ params }, null, 2));
    console.error(`\n# seeded ${params.length} scalar param(s) from get_def(); fill in min/max/doc and merge into plugin.json's "manifest".`);
    process.exit(0);
}

// --- check mode: compare scalar get_def keys vs manifest.params names ---
if (!existsSync(checkPath)) die('plugin.json not found: ' + checkPath);
let pj;
try { pj = JSON.parse(readFileSync(checkPath, 'utf8')); } catch (e) { die('cannot parse ' + checkPath + ': ' + e.message); }
const declared = ((pj.manifest && Array.isArray(pj.manifest.params)) ? pj.manifest.params : [])
    .map((p) => p && p.name).filter(Boolean);

const defSet = new Set(scalarKeys), decSet = new Set(declared);
const missing = scalarKeys.filter((k) => !decSet.has(k));      // emitted but not declared
const extra = declared.filter((k) => !defSet.has(k));          // declared but not emitted

if (!missing.length && !extra.length) {
    console.log(`OK — manifest.params matches get_def() (${scalarKeys.length} scalar param(s)).`);
    process.exit(0);
}
console.error(`DRIFT in ${checkPath}:`);
if (missing.length) console.error('  emitted by get_def() but missing from manifest.params: ' + missing.join(', '));
if (extra.length)   console.error('  declared in manifest.params but get_def() does not emit: ' + extra.join(', '));
process.exit(1);
