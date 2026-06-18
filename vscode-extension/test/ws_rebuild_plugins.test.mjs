// ws_rebuild_plugins.test.mjs — `build: cmake` plugins + cmd:rebuild_plugins.
//
// A cmake/prebuilt plugin owns its own CMake build (for external libs / CUDA).
// The backend never cl.exe-compiles it; `rebuild_plugins` runs its CMake, then
// unload→build→load swaps the DLL (preserving instance state) — and skips
// plugins whose source didn't change. This drives that end to end:
//   open_project (plugin registered, not built) → rebuild (rebuilt) →
//   rebuild again (unchanged) → edit source → rebuild (rebuilt).
//
// Requires cmake + OpenCV on the dev box (same as the standalone-plugin path).
// If cmake isn't on PATH the test skips rather than failing.

import test from 'node:test';
import assert from 'node:assert/strict';
import { resolve, dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const slash = (s) => s.split('\\').join('/');

function cmakeAvailable() {
    try { execFileSync('cmake', ['--version'], { stdio: 'ignore' }); return true; }
    catch { return false; }
}

const PLUGIN_JSON = JSON.stringify({
    name: 'verprobe', build: 'cmake', dll: 'verprobe.dll', factory: 'xi_plugin_create',
});

const CMAKELISTS = `cmake_minimum_required(VERSION 3.16)
project(verprobe CXX C)
set(CMAKE_CXX_STANDARD 20)
if(NOT DEFINED XINSP2_ROOT AND DEFINED ENV{XINSP2_ROOT})
  set(XINSP2_ROOT $ENV{XINSP2_ROOT})
endif()
include(\${XINSP2_ROOT}/sdk/cmake/xinsp2_plugin.cmake)
xinsp2_add_plugin(verprobe plugin.cpp)
`;

const PLUGIN_CPP = (v) => `#include <xi/xi_abi.hpp>
class VerProbe : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    xi::Record process(const xi::Record& in) override {
        return xi::Record().set("v", ${v});
    }
};
XI_PLUGIN_IMPL(VerProbe)
`;

// cmd:rebuild_plugins -> the item for `verprobe` from data.plugins[].
async function rebuild(c, id) {
    c.send({ type: 'cmd', id, name: 'rebuild_plugins' });
    for (;;) {
        const m = await c.nextText(180000);   // cmake configure+compile can be slow cold
        if (m.type === 'rsp' && m.id === id) {
            assert.ok(m.ok, 'rebuild_plugins rsp ok');
            const items = m.data?.plugins ?? [];
            return items.find((i) => i.plugin === 'verprobe');
        }
    }
}

test('build:cmake plugin builds + reloads via rebuild_plugins, skips when unchanged', async (t) => {
    if (!cmakeAvailable()) { t.skip('cmake not on PATH'); return; }

    const proj = join(tmpdir(), 'xi_rebuild_' + process.pid + '_' + Date.now());
    const pdir = join(proj, 'plugins', 'verprobe');
    mkdirSync(pdir, { recursive: true });
    writeFileSync(join(proj, 'project.json'), JSON.stringify({ name: 'rebuild_test',
        plugins: { verprobe: { path: 'verprobe', compile: true } } }));
    writeFileSync(join(proj, 'inspect.cpp'), '#include <xi/xi.hpp>\nXI_SCRIPT_EXPORT void xi_inspect_entry(int){}\n');
    writeFileSync(join(pdir, 'plugin.json'), PLUGIN_JSON);
    writeFileSync(join(pdir, 'CMakeLists.txt'), CMAKELISTS);
    writeFileSync(join(pdir, 'plugin.cpp'), PLUGIN_CPP(1));

    try {
        await withBackend(async (c) => {
            // open_project: verprobe is registered but NOT built yet.
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { path: slash(proj) } });
            for (;;) { const m = await c.nextText(); if (m.type === 'rsp' && m.id === 1) { assert.ok(m.ok, 'open ok'); break; } }

            // First rebuild: cmake builds verprobe.dll, backend loads it.
            const r1 = await rebuild(c, 10);
            assert.ok(r1, 'verprobe present in rebuild report');
            assert.equal(r1.status, 'rebuilt', `first rebuild = rebuilt (got ${r1.status}: ${r1.detail || ''})`);

            // list_plugins now shows verprobe as a known plugin.
            c.send({ type: 'cmd', id: 11, name: 'list_plugins' });
            for (;;) {
                const m = await c.nextText();
                if (m.type === 'rsp' && m.id === 11) {
                    const list = m.data ?? m;
                    const vp = (Array.isArray(list) ? list : []).find((p) => p && p.name === 'verprobe');
                    assert.ok(vp, 'verprobe listed after rebuild');
                    // the prebuilt flag drives the tree's per-item "Rebuild this plugin"
                    assert.equal(vp.prebuilt, true, 'verprobe reported prebuilt:true in list_plugins');
                    break;
                }
            }

            // Second rebuild, no source change: the mtime gate skips it.
            const r2 = await rebuild(c, 12);
            assert.equal(r2.status, 'unchanged', `unchanged when source untouched (got ${r2.status})`);

            // Edit the source → rebuild detects the change and rebuilds again.
            await new Promise((res) => setTimeout(res, 1100));   // ensure mtime advances
            writeFileSync(join(pdir, 'plugin.cpp'), PLUGIN_CPP(2));
            const r3 = await rebuild(c, 13);
            assert.equal(r3.status, 'rebuilt', `rebuilt after source edit (got ${r3.status}: ${r3.detail || ''})`);
        });
    } finally {
        try { rmSync(proj, { recursive: true, force: true }); } catch {}
    }
});
