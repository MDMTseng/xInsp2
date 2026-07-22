#!/usr/bin/env node
//
// build_release.mjs — assemble a self-contained xInsp2 release zip.
//
// Output layout:
//   release/xinsp2-<version>-win-x64/
//     bin/                xinsp-backend.exe + DLLs
//     plugins/            built plugins   (runtime name — the backend
//                         auto-scans <cwd>/plugins; the SOURCE tree is toolbox/)
//     sdk/                template + examples + cmake module
//     extension/          xinsp2-<version>.vsix
//     docs/               FRAMEWORK / NewDeal / protocol
//     INSTALL.md
//     README.md
//
// Usage:  node tools/build_release.mjs [--no-rebuild] [--skip-vsix] [--manifest-only]
//
//   --manifest-only  skip the (heavy) backend/plugin/VSIX build and just write
//                    compat-manifest.json into the staging dir — for inspecting
//                    or gating the manifest without cutting a full release.
//
import { execSync, spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, cpSync, readFileSync, writeFileSync, rmSync, readdirSync, statSync, createWriteStream } from 'node:fs';
import { dirname, resolve, join, basename } from 'node:path';
import { fileURLToPath } from 'node:url';
import { buildManifest, parseAbi } from './compat_manifest.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(__dirname, '..');

const args = process.argv.slice(2);
const NO_REBUILD = args.includes('--no-rebuild');
const SKIP_VSIX  = args.includes('--skip-vsix');
const MANIFEST_ONLY = args.includes('--manifest-only');

// compat-manifest.json — a machine-readable statement of what this artifact IS
// and what it claims compatibility with, written from the real version sources
// at build time (see tools/compat_manifest.mjs; external review 06). Every field
// is read from source — nothing hand-typed here.
function writeCompatManifest(stageDir) {
    const manifest = buildManifest(ROOT);
    const dst = join(stageDir, 'compat-manifest.json');
    writeFileSync(dst, JSON.stringify(manifest, null, 2) + '\n');
    console.log(`  compat-manifest.json → ${dst}`);
    return manifest;
}

// ---- Read version from CMakeLists.txt ----
function readVersion() {
    const txt = readFileSync(join(ROOT, 'backend', 'CMakeLists.txt'), 'utf8');
    const m = txt.match(/set\(XINSP2_VERSION\s+"([^"]+)"\)/);
    if (!m) throw new Error('XINSP2_VERSION not found in backend/CMakeLists.txt');
    return m[1];
}
const VERSION = readVersion();
const ABI_VERSION = parseAbi(ROOT).version;   // for the INSTALL.md rollback note
const RELEASE_NAME = `xinsp2-${VERSION}-win-x64`;
const STAGE = join(ROOT, 'release', RELEASE_NAME);
console.log(`> xInsp2 ${VERSION} → ${STAGE}`);

// --manifest-only: generate just the compat manifest (no heavy build). Writes it
// into the staging dir if the dir already exists, else prints it to stdout.
if (MANIFEST_ONLY) {
    const manifest = buildManifest(ROOT);
    const json = JSON.stringify(manifest, null, 2) + '\n';
    if (existsSync(STAGE)) {
        writeFileSync(join(STAGE, 'compat-manifest.json'), json);
        console.log(`compat-manifest.json written into ${STAGE}`);
    }
    process.stdout.write(json);
    process.exit(0);
}

function sh(cmd, opts = {}) {
    console.log(`$ ${cmd}`);
    execSync(cmd, { stdio: 'inherit', cwd: ROOT, ...opts });
}

function copyDir(src, dst, filter) {
    cpSync(src, dst, {
        recursive: true,
        filter: (s) => {
            const rel = s.slice(src.length).replace(/\\/g, '/');
            // Skip build dirs, node_modules, and gitignored junk.
            if (/(^|\/)(build|node_modules|\.git|\.vs|out)(\/|$)/.test(rel)) return false;
            if (/\.(obj|ilk|exp|tlog|pdb|recipe|cache|log)$/.test(rel)) return false;
            return filter ? filter(rel) : true;
        },
    });
}

// ---- 1. (Re)build backend Release if needed ----
const backendBuild = join(ROOT, 'backend', 'build');
const exePath = join(backendBuild, 'Release', 'xinsp-backend.exe');
if (!NO_REBUILD || !existsSync(exePath)) {
    if (!existsSync(backendBuild)) mkdirSync(backendBuild, { recursive: true });
    sh(`cmake -S backend -B backend/build -A x64 -DXINSP2_HAS_OPENCV=ON -DXINSP2_HAS_TURBOJPEG=ON -DXINSP2_HAS_IPP=ON`);
    sh(`cmake --build backend/build --config Release --target xinsp_backend xinsp_runner`);
}

// ---- 2. Build plugins (lib only — DLLs already land next to plugin.json) ----
const pluginsBuild = join(ROOT, 'toolbox', 'build');
if (!NO_REBUILD || !existsSync(pluginsBuild)) {
    if (!existsSync(pluginsBuild)) mkdirSync(pluginsBuild, { recursive: true });
    sh(`cmake -S toolbox -B toolbox/build -A x64`);
    sh(`cmake --build toolbox/build --config Release`);
}

// ---- 3. Build VS Code extension (esbuild bundle) and package as VSIX ----
const extDir = join(ROOT, 'vscode-extension');
let vsixPath = null;
if (!SKIP_VSIX) {
    sh(`npm run build`, { cwd: extDir });
    // vsce package — install if missing.
    const vscePath = join(extDir, 'node_modules', '.bin', 'vsce.cmd');
    if (!existsSync(vscePath)) {
        sh(`npm install --save-dev @vscode/vsce`, { cwd: extDir });
    }
    sh(`"${vscePath}" package --no-dependencies --out xinsp2-${VERSION}.vsix`, { cwd: extDir });
    vsixPath = join(extDir, `xinsp2-${VERSION}.vsix`);
}

// ---- 4. Assemble staging dir ----
if (existsSync(STAGE)) rmSync(STAGE, { recursive: true, force: true });
mkdirSync(STAGE, { recursive: true });
mkdirSync(join(STAGE, 'bin'), { recursive: true });
mkdirSync(join(STAGE, 'plugins'), { recursive: true });
mkdirSync(join(STAGE, 'sdk'), { recursive: true });
mkdirSync(join(STAGE, 'extension'), { recursive: true });
mkdirSync(join(STAGE, 'docs'), { recursive: true });

// bin/  — exe + auto-deployed runtime DLLs
const binSrc = join(backendBuild, 'Release');
for (const f of readdirSync(binSrc)) {
    if (/\.(exe|dll)$/.test(f)) {
        cpSync(join(binSrc, f), join(STAGE, 'bin', f));
    }
}

// toolbox/ (source) -> plugins/ (bundle): the backend auto-scans <cwd>/plugins,
// so the shipped folder keeps the runtime name. Skip build dirs.
// Not every folder under toolbox/ is a plugin. `tests` is the cross-plugin ctest
// suite and `pluginlets` is a compile-time source library that plugin authors
// #include — neither is something the backend should find while scanning
// <cwd>/plugins, and neither belongs in a customer bundle.
const NOT_A_PLUGIN = new Set(['build', 'tests', 'pluginlets']);
for (const p of readdirSync(join(ROOT, 'toolbox'))) {
    const ps = join(ROOT, 'toolbox', p);
    if (!statSync(ps).isDirectory() || NOT_A_PLUGIN.has(p)) continue;
    copyDir(ps, join(STAGE, 'plugins', p));
}

// sdk/  — full SDK, no build artifacts
copyDir(join(ROOT, 'sdk'), join(STAGE, 'sdk'));

// extension/
if (vsixPath && existsSync(vsixPath)) {
    cpSync(vsixPath, join(STAGE, 'extension', basename(vsixPath)));
}

// docs/
for (const d of ['FRAMEWORK.md', 'NewDeal.md', 'STATUS.md', 'DEV_PLAN.md']) {
    const s = join(ROOT, d);
    if (existsSync(s)) cpSync(s, join(STAGE, 'docs', d));
}
mkdirSync(join(STAGE, 'docs', 'protocol'), { recursive: true });
cpSync(join(ROOT, 'protocol', 'messages.md'), join(STAGE, 'docs', 'protocol', 'messages.md'));

// Top-level README + INSTALL
cpSync(join(ROOT, 'README.md'), join(STAGE, 'README.md'));
writeFileSync(join(STAGE, 'INSTALL.md'),
`# xInsp2 ${VERSION} — Quick install

## 1. Install the VS Code extension

In VS Code: \`Extensions → … → Install from VSIX…\` and pick
\`extension/xinsp2-${VERSION}.vsix\`.

## 2. Place the runtime

Copy the **bin/** folder somewhere stable (e.g. \`C:\\xinsp2\`).

The extension auto-spawns \`bin/xinsp-backend.exe\`. If you placed it
elsewhere, set the absolute path in VS Code settings:

- \`xinsp2.backendExe\` = \`C:\\xinsp2\\bin\\xinsp-backend.exe\`

For remote mode (factory PC), set:
- \`xinsp2.remoteUrl\` = \`ws://factory:7823\`
- \`xinsp2.authSecret\` = \`<your-shared-secret>\`

…and start the server side with \`xinsp-backend.exe --host=0.0.0.0
--auth=<secret>\`.

## 3. Plugins

The bundled \`plugins/\` folder is loaded automatically when the backend
starts in this directory. To add your own, point
\`xinsp2.extraPluginDirs\` at the parent folder.

## 4. Write your first plugin

\`\`\`bash
node sdk\\scaffold.mjs C:\\dev\\my_plugins\\my_plugin
cd C:\\dev\\my_plugins\\my_plugin
cmake -S . -B build -A x64
cmake --build build --config Release
\`\`\`

See \`sdk/GETTING_STARTED.md\` for the full walkthrough.

## 5. Headless runner

For production deployment without VS Code:

\`\`\`bash
bin\\xinsp-runner.exe path\\to\\project --frames=1000 --output=report.json
\`\`\`

## What's where

| Folder | Contents |
|--------|----------|
| \`bin/\` | xinsp-backend.exe, xinsp-runner.exe, plus all runtime DLLs |
| \`plugins/\` | 7 shipped plugins — drop more folders here to extend |
| \`sdk/\` | Plugin SDK: scaffold, template, examples, cmake module |
| \`extension/\` | VS Code extension VSIX |
| \`docs/\` | Framework reference, dev plan, protocol spec |
| \`compat-manifest.json\` | Machine-readable identity: versions, plugin-ABI, WS protocol, known-compatible set |

## Identity & rollback

\`compat-manifest.json\` at the root of this artifact records exactly what this
build is — backend version, git sha, plugin-ABI + min-compat, WS-protocol abi +
command count + contract schemas — and the package set it was tested against, all
read from source at build time. A target machine can answer "what am I running,
and what does it claim compatibility with" from this file alone, with no network
or source tree.

Rollback here is zip-swap: replace the deployed folder with an older artifact.
Because each artifact carries its own \`compat-manifest.json\`, the version identity
travels with the zip — read the manifest of the build you are rolling to. One
caveat crossing a **plugin-ABI** boundary (this build is v${ABI_VERSION}): a
plugin binary only loads on the ABI it was built against, so plugins are part of
the release unit, not independently rollable — a host rolled across an ABI break
refuses plugins built for the other side (by design, loudly). See
\`docs/reference/ws-protocol.md\` and \`README.md\` §Versioning.

## Verify

\`\`\`bash
bin\\xinsp-backend.exe --version
\`\`\`
should print \`xinsp-backend ${VERSION} (<commit-hash>)\`.
`);

// compat-manifest.json — machine-readable identity of this artifact.
writeCompatManifest(STAGE);

console.log(`\nstaged at ${STAGE}`);

// ---- 5. Zip it ----
const zipPath = `${STAGE}.zip`;
if (existsSync(zipPath)) rmSync(zipPath);
sh(`powershell -NoProfile -Command "Compress-Archive -Path '${STAGE}\\*' -DestinationPath '${zipPath}' -Force"`);

const zipBytes = statSync(zipPath).size;
console.log(`\n✓ release: ${zipPath}  (${(zipBytes / 1024 / 1024).toFixed(1)} MB)`);
