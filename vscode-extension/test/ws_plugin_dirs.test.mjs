// ws_plugin_dirs.test.mjs — project.json `plugin_dirs` + `plugins` coordinates.
//
// A project can keep plugins OUTSIDE its folder: `plugin_dirs` is an ordered list
// of search roots (portable: relative / ${ENV} / ~), and each `plugins` entry's
// `path` is resolved against them first-match-wins (makefile / $PATH style). This
// keeps the committed project.json machine-independent — the absolute roots live
// in env vars, the coordinates stay in the file.
//
// Verifies: a coordinate resolves + registers (shows in list_plugins); an
// unresolved coordinate becomes an open warning listing the roots searched;
// relative roots expand against the project folder; ${ENV} roots expand.

import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const rsp = async (c, id) => { for (;;) { const m = await c.nextText(60000); if (m.type === 'rsp' && m.id === id) return m; } };

test('project.json plugin_dirs resolves external plugin coordinates', async () => {
    const base = join(tmpdir(), 'xi_pdirs_' + process.pid + '_' + Date.now());
    const reg = join(base, 'reg');
    const proj = join(base, 'proj');
    // an external plugin in an author/name registry layout (manifest is enough to register)
    mkdirSync(join(reg, 'acme', 'extwidget'), { recursive: true });
    writeFileSync(join(reg, 'acme', 'extwidget', 'plugin.json'),
        JSON.stringify({ name: 'extwidget', dll: 'extwidget.dll', description: 'external via plugin_dirs' }));
    // a second plugin reachable via an ${ENV} root
    const envreg = join(base, 'envreg');
    mkdirSync(join(envreg, 'gadget'), { recursive: true });
    writeFileSync(join(envreg, 'gadget', 'plugin.json'),
        JSON.stringify({ name: 'gadget', dll: 'gadget.dll', description: 'external via ${ENV} root' }));

    mkdirSync(proj, { recursive: true });
    writeFileSync(join(proj, 'inspect.cpp'),
        '#include <xi/xi.hpp>\nXI_SCRIPT_EXPORT void xi_inspect_entry(int){}\n');
    writeFileSync(join(proj, 'project.json'), JSON.stringify({
        name: 'pdirs_test',
        script: 'inspect.cpp',
        plugin_dirs: [slash(reg), '${XI_TEST_REG}', './local_plugins'],
        plugins: {
            extwidget: { path: 'acme/extwidget' },     // resolves in reg
            gadget:    { path: 'gadget' },              // resolves in ${XI_TEST_REG}
            missing:   { path: 'nope/ghost' },          // resolves nowhere -> warning
        },
    }));

    process.env.XI_TEST_REG = slash(envreg);   // inherited by the spawned backend
    try {
        await withBackend(async (c) => {
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { path: slash(proj) } });
            assert.ok((await rsp(c, 1)).ok, 'open ok');

            c.send({ type: 'cmd', id: 2, name: 'list_plugins' });
            const list = (await rsp(c, 2)).data;
            const names = (Array.isArray(list) ? list : []).map((p) => p.name);
            assert.ok(names.includes('extwidget'), 'coordinate resolved against a relative-listed absolute root');
            assert.ok(names.includes('gadget'), 'coordinate resolved against a ${ENV} root');

            c.send({ type: 'cmd', id: 3, name: 'open_project_warnings' });
            const warns = JSON.stringify((await rsp(c, 3)).data);
            assert.ok(warns.includes('nope/ghost'), 'unresolved coordinate is warned');
            assert.ok(warns.includes('local_plugins'), 'warning lists the searched roots (incl. the project-relative one)');
        });
    } finally {
        delete process.env.XI_TEST_REG;
        try { rmSync(base, { recursive: true, force: true }); } catch {}
    }
});
