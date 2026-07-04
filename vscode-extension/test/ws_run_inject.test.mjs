// ws_run_inject.test.mjs — cmd:run injects a one-shot sealed PACK (frame + meta)
// into current_trigger(), so a headless run feeds the script the same way a
// pack-mode source's emit would — no source plugin, no continuous mode.
//
// v12 (THE CUT): `meta`'s top-level scalars become flat pack entries
// (str/i64/f64/bool; nested objects are NOT injected) and the script reads them
// via t.pack() — t.meta()/the Record meta plane are gone. The vars frame is
// gone too, so the script surfaces what it saw through the ONE per-run verdict
// (xi::result → the run_result event): code = the injected "recipe", msg = the
// injected "command".

import test from 'node:test';
import assert from 'node:assert/strict';
import { writeFileSync, rmSync, mkdirSync } from 'node:fs';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) { xi::result(-100, "inactive"); return; }
    auto f = t.pack();
    if (!f) { xi::result(-101, "no pack"); return; }
    long long recipe = f.get_i64("recipe").value_or(-1);
    std::string cmd(f.get_str("command").value_or("missing"));
    xi::result((int)recipe, cmd);
}
`;

async function rsp(c, id) {
    for (;;) { const m = await c.nextText(120000); if (m.type === 'rsp' && m.id === id) return m; }
}
async function runResult(c) {
    for (;;) {
        const m = await c.nextText(120000);
        if (m.type === 'event' && m.name === 'run_result') return m.data;
    }
}

test('cmd:run injects inline meta as pack entries readable via t.pack()', { timeout: 120000 }, async () => {
    const dir = join(tmpdir(), `xi_inject_${Date.now()}`);
    mkdirSync(dir, { recursive: true });
    const path = join(dir, 'inspect.cpp');
    writeFileSync(path, SCRIPT);
    try {
        await withBackend(async (c) => {
            await c.nextText(); // hello
            c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(path) } });
            assert.equal((await rsp(c, 1)).ok, true, 'compile ok');

            // Run WITH inline meta → current_trigger active, scalars ride the pack.
            c.send({ type: 'cmd', id: 2, name: 'run', args: { meta: { recipe: 42, command: 'topcheck' } } });
            assert.equal((await rsp(c, 2)).ok, true, 'run ok');
            const r1 = await runResult(c);
            console.log('injected-meta run_result:', JSON.stringify(r1));
            assert.equal(r1.code, 42, 'recipe 42 surfaced via t.pack().get_i64');
            assert.equal(r1.msg, 'topcheck', 'command topcheck surfaced via t.pack().get_str');

            // Control: a plain run carries no pack → trigger inactive.
            c.send({ type: 'cmd', id: 3, name: 'run' });
            assert.equal((await rsp(c, 3)).ok, true, 'plain run ok');
            const r2 = await runResult(c);
            console.log('plain-run run_result:', JSON.stringify(r2));
            assert.equal(r2.code, -100, 'plain run: trigger inactive, no injected pack');
        });
    } finally {
        try { rmSync(dir, { recursive: true }); } catch {}
    }
});
