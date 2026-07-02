#!/usr/bin/env node
//
// compat_manifest.mjs — build the in-artifact `compat-manifest.json`.
//
// A release zip is the rollback unit, but it carried no machine-readable
// statement of WHAT it is (which backend / ABI / protocol / extension) or what
// it claims compatibility with — the "known-compatible matrix" lived only in
// README prose (external review 06, "Release packaging & rollback" / "Version
// identity coherence", grade C). This module reads every version identity from
// its REAL source at build time (nothing hand-typed) and assembles a manifest
// build_release.mjs drops into the artifact root, so a target machine can answer
// "what am I running, and what does it claim compatibility with" from the zip
// alone.
//
// Sources of truth (each parsed here, never duplicated):
//   backend version   backend/CMakeLists.txt   set(XINSP2_VERSION "…")
//   plugin ABI        backend/include/xi/xi_abi.h   XI_ABI_VERSION / XI_ABI_MIN_COMPAT
//   WS protocol abi   backend/src/service_main.cpp   hello event `"abi":N`
//   WS command count  backend/src/service_main.cpp   g_cmd_table entries
//   contract schemas  contract/schemas/*.schema.json
//   extension version vscode-extension/package.json
//   ui-components     ui-components/package.json
//   python client     tools/xinsp2_py/pyproject.toml
//   known-compatible  tools/compat-matrix.json  (the ONE authority for the SET)
//   built_from        git rev-parse HEAD
//
// Usage (standalone / --manifest-only path):
//   node tools/compat_manifest.mjs [--out <path>] [--pretty]
//     no --out  → prints the manifest JSON to stdout
//     --out P   → writes it to P
//
import { readFileSync, writeFileSync, existsSync, readdirSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
export const ROOT = resolve(__dirname, '..');

// A sanity floor for the WS command table, mirroring WS_TABLE_MIN in
// tools/check_doc_coverage.py: if the parse suddenly yields far fewer commands
// than the table really has, the format changed under us and we refuse to stamp
// a bogus count rather than silently ship one.
export const WS_TABLE_MIN = 40;

function read(p) {
    return readFileSync(p, 'utf8');
}

// ---- individual source-of-truth parsers ----------------------------------

export function parseBackendVersion(root = ROOT) {
    const txt = read(join(root, 'backend', 'CMakeLists.txt'));
    const m = txt.match(/set\(XINSP2_VERSION\s+"([^"]+)"\)/);
    if (!m) throw new Error('XINSP2_VERSION not found in backend/CMakeLists.txt');
    return m[1];
}

export function parseAbi(root = ROOT) {
    const txt = read(join(root, 'backend', 'include', 'xi', 'xi_abi.h'));
    const ver = txt.match(/#define\s+XI_ABI_VERSION\s+(\d+)/);
    const min = txt.match(/#define\s+XI_ABI_MIN_COMPAT\s+(\d+)/);
    if (!ver) throw new Error('XI_ABI_VERSION not found in xi_abi.h');
    if (!min) throw new Error('XI_ABI_MIN_COMPAT not found in xi_abi.h');
    return { version: Number(ver[1]), min_compat: Number(min[1]) };
}

export function parseWsAbi(root = ROOT) {
    // The hello event embeds the WS-protocol version as `"abi":N` in a raw
    // string literal in send_hello(). Distinct from the plugin ABI above.
    const txt = read(join(root, 'backend', 'src', 'service_main.cpp'));
    const m = txt.match(/"abi"\s*:\s*(\d+)/);
    if (!m) throw new Error('WS protocol `"abi":N` not found in service_main.cpp');
    return Number(m[1]);
}

// Replicates the g_cmd_table extraction in tools/check_doc_coverage.py
// (extract_ws_commands) — cross-language, so it cannot import that Python, but
// it uses the SAME anchoring: isolate the `g_cmd_table = { … };` span, then grab
// each `{"literal",` key. Kept in lock-step with the Python sibling; both guard
// on WS_TABLE_MIN so a table restructure trips loudly here too.
export function parseWsCommands(root = ROOT) {
    const txt = read(join(root, 'backend', 'src', 'service_main.cpp'));
    const start = txt.match(/g_cmd_table\s*=\s*\{/);
    if (!start) {
        throw new Error(
            'could not find `g_cmd_table = {` in service_main.cpp — the WS command '
            + 'dispatch table moved or was renamed. Update parseWsCommands() to match '
            + '(mirror of extract_ws_commands in check_doc_coverage.py).');
    }
    let body = txt.slice(start.index + start[0].length);
    const end = body.indexOf('};');
    if (end !== -1) body = body.slice(0, end);
    const cmds = new Set();
    for (const m of body.matchAll(/\{\s*"([a-z_]+)"\s*,/g)) cmds.add(m[1]);
    if (cmds.size < WS_TABLE_MIN) {
        throw new Error(
            `extracted only ${cmds.size} WS command(s) from g_cmd_table `
            + `(expected >= ${WS_TABLE_MIN}) — the table format likely changed and the `
            + 'entry regex no longer matches; fix parseWsCommands(). Refusing to stamp '
            + 'an implausibly small command count.');
    }
    return [...cmds].sort();
}

export function readSchemas(root = ROOT) {
    const dir = join(root, 'contract', 'schemas');
    if (!existsSync(dir)) return [];
    return readdirSync(dir)
        .filter((f) => f.endsWith('.schema.json'))
        .sort()
        .map((f) => {
            let id = null;
            try {
                id = JSON.parse(read(join(dir, f))).$id ?? null;
            } catch { /* leave id null; the self-check flags unreadable schemas */ }
            return { file: f, $id: id };
        });
}

export function readPackageVersion(root, relPath) {
    return JSON.parse(read(join(root, relPath))).version;
}

export function parsePyprojectVersion(root = ROOT) {
    const txt = read(join(root, 'tools', 'xinsp2_py', 'pyproject.toml'));
    // The [project] version key — first `version = "…"` under the top table.
    const m = txt.match(/^\s*version\s*=\s*"([^"]+)"/m);
    if (!m) throw new Error('version not found in tools/xinsp2_py/pyproject.toml');
    return m[1];
}

export function readMatrix(root = ROOT) {
    return JSON.parse(read(join(root, 'tools', 'compat-matrix.json')));
}

export function gitSha(root = ROOT) {
    try {
        return execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root })
            .toString().trim();
    } catch {
        return null;   // building outside a git checkout — self-check flags null
    }
}

// ---- assembly ------------------------------------------------------------

export function buildManifest(root = ROOT, opts = {}) {
    const backend = parseBackendVersion(root);
    const abi = parseAbi(root);
    const matrix = readMatrix(root).known_compatible;

    return {
        artifact: {
            name: `xinsp2-${backend}-win-x64`,
            version: backend,
        },
        built_from: opts.gitSha ?? gitSha(root),
        backend_version: backend,
        plugin_abi: {
            version: abi.version,
            min_compat: abi.min_compat,
        },
        ws_protocol: {
            abi: parseWsAbi(root),
            command_count: parseWsCommands(root).length,
            schemas: readSchemas(root),
        },
        known_compatible: {
            extension: {
                version: readPackageVersion(root, 'vscode-extension/package.json'),
                range: readMatrix(root).compat_ranges?.extension ?? null,
            },
            ui_components: readPackageVersion(root, 'ui-components/package.json'),
            python_client: {
                version: parsePyprojectVersion(root),
                range: readMatrix(root).compat_ranges?.python_client ?? null,
            },
            plugin_abi: matrix.plugin_abi,
        },
        timestamp: opts.timestamp ?? new Date().toISOString(),
    };
}

// ---- CLI -----------------------------------------------------------------

function isMain() {
    return process.argv[1]
        && resolve(process.argv[1]) === fileURLToPath(import.meta.url);
}

if (isMain()) {
    const args = process.argv.slice(2);
    const outIdx = args.indexOf('--out');
    const out = outIdx !== -1 ? args[outIdx + 1] : null;
    const manifest = buildManifest();
    const json = JSON.stringify(manifest, null, 2) + '\n';
    if (out) {
        writeFileSync(out, json);
        console.log(`compat-manifest written to ${out}`);
    } else {
        process.stdout.write(json);
    }
}
