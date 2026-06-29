// ws_run_inject.test.mjs — Stage 1b: cmd:run injects a record (frame + meta)
// into current_trigger(), so a headless run feeds the script the same way a
// source's emit_record would — no source plugin, no continuous mode.
//
// A tiny inline script reads current_trigger().meta() and VARs the fields. We
// compile it, then `cmd:run` with an inline `meta` object and assert the script
// saw it (trigger active + recipe/command surfaced). A plain `cmd:run` (no meta)
// is the control: the trigger stays inactive.

import test from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync, rmSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
XI_SCRIPT_EXPORT
void xi_inspect_entry(int) {
    auto t = xi::current_trigger();
    VAR(inj_active, t.is_active() ? 1 : 0);
    if (!t.is_active()) return;
    auto m = t.meta();
    VAR(inj_recipe, m["recipe"].as_int(-1));
    VAR(inj_cmd,    m["command"].as_string());
}
`;

async function rsp(c, id) {
    for (;;) { const m = await c.nextText(120000); if (m.type === 'rsp' && m.id === id) return m; }
}
async function vars(c) {
    for (;;) { const m = await c.nextText(120000); if (m.type === 'vars') return m; }
}

test('cmd:run injects inline meta into current_trigger().meta()', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (cmd:run meta injection still works; observed via vars)' }, async () => {
    const dir = join(tmpdir(), `xi_inject_${Date.now()}`);
    mkdirSync(dir, { recursive: true });
    const path = join(dir, 'inspect.cpp');
    writeFileSync(path, SCRIPT);
    try {
        await withBackend(async (c) => {
            await c.nextText(); // hello
            c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(path) } });
            assert.equal((await rsp(c, 1)).ok, true, 'compile ok');

            // Run WITH inline meta → current_trigger active + meta surfaces.
            c.send({ type: 'cmd', id: 2, name: 'run', args: { meta: { recipe: 42, command: 'topcheck' } } });
            assert.equal((await rsp(c, 2)).ok, true, 'run ok');
            const s = JSON.stringify((await vars(c)).items);
            console.log('injected-meta vars:', s);
            assert.ok(s.includes('inj_recipe'), 'meta-derived var present (trigger was active)');
            assert.ok(s.includes('42'),         'recipe 42 surfaced via current_trigger().meta()');
            assert.ok(s.includes('topcheck'),   'command topcheck surfaced');

            // Control: a plain run carries no record → trigger inactive, no meta vars.
            c.send({ type: 'cmd', id: 3, name: 'run' });
            assert.equal((await rsp(c, 3)).ok, true, 'plain run ok');
            const s2 = JSON.stringify((await vars(c)).items);
            console.log('plain-run vars:', s2);
            assert.ok(!s2.includes('inj_recipe'), 'plain run: trigger inactive, no injected meta');
        });
    } finally {
        try { rmSync(dir, { recursive: true }); } catch {}
    }
});
