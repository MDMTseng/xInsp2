// ws_plugin_dirs_compile.test.mjs — per-plugin `compile` flag on plugin_dirs refs.
//
// By default a plugin_dirs-resolved plugin is only REGISTERED (must be prebuilt
// or build:cmake). A `plugins` entry with `"compile": true` (per-plugin; the
// project-level `plugin_dirs_compile` is just the default) makes the backend treat
// that resolved folder exactly like a <project>/plugins/ one — cl.exe-compile a
// source plugin and load it as a TRUSTED project plugin. The "plugin toolbox" dev
// case: point plugin_dirs at a folder of WIP source plugins and compile the ones
// you're working on (recompile / rebuild from VS Code).
//
// Verifies: an EXTERNAL source plugin (no prebuilt DLL, no CMakeLists) compiles
// and loads only because the switch is on, and is reported origin:"project".
// Needs cl.exe (cold compile) — slow, like the other compile-path suites.

import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const rsp = async (c, id) => { for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; } };

const PLUGIN_CPP = `#include <xi/xi_abi.hpp>
class Dbl : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    xi::Record process(const xi::Record& in) override {
        return xi::Record().set("result", in["value"].as_double(0.0) * 2.0);
    }
};
XI_PLUGIN_IMPL(Dbl)
`;

test('plugin_dirs_compile cl.exe-compiles an external source plugin', async () => {
    const base = join(tmpdir(), 'xi_tbx_' + process.pid + '_' + Date.now());
    const tb = join(base, 'toolbox', 'x', 'extdbl');
    const proj = join(base, 'proj');
    mkdirSync(join(tb, 'src'), { recursive: true });
    // external SOURCE plugin: no DLL, no CMakeLists — only loads if compiled.
    writeFileSync(join(tb, 'plugin.json'),
        JSON.stringify({ name: 'extdbl', dll: 'extdbl.dll', description: 'external source via plugin_dirs' }));
    writeFileSync(join(tb, 'src', 'plugin.cpp'), PLUGIN_CPP);

    mkdirSync(proj, { recursive: true });
    writeFileSync(join(proj, 'inspect.cpp'),
        '#include <xi/xi.hpp>\nXI_SCRIPT_EXPORT void xi_inspect_entry(int){}\n');
    writeFileSync(join(proj, 'project.json'), JSON.stringify({
        name: 'tbx_test',
        script: 'inspect.cpp',
        plugin_dirs: [slash(join(base, 'toolbox'))],
        // per-plugin compile flag (not a project-wide switch)
        plugins: { extdbl: { path: 'x/extdbl', compile: true } },
    }));

    try {
        await withBackend(async (c) => {
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { path: slash(proj) } });
            assert.ok((await rsp(c, 1)).ok, 'open ok');

            c.send({ type: 'cmd', id: 2, name: 'list_plugins' });
            const list = (await rsp(c, 2)).data;
            const p = (Array.isArray(list) ? list : []).find((x) => x.name === 'extdbl');
            assert.ok(p, 'extdbl resolved');
            assert.equal(p.loaded, true, 'extdbl compiled + loaded thanks to plugin_dirs_compile');
            assert.equal(p.origin, 'project', 'compiled external plugin is treated as a trusted project plugin');
        });
    } finally {
        try { rmSync(base, { recursive: true, force: true }); } catch {}
    }
});
