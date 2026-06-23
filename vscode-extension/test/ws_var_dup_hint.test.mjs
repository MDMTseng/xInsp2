// ws_var_dup_hint.test.mjs — a duplicate VAR(name) is a C++ redefinition (VAR
// declares a variable). The backend recognises the redefinition error and
// appends a clear, actionable hint pointing at the VAR() line numbers, so the
// recurring "mysterious macro error" papercut surfaces plainly.

import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
async function rsp(c, id) { for (;;) { const m = await c.nextText(120000); if (m.type === 'rsp' && m.id === id) return m; } }

test('duplicate VAR(name) compile error gets a clear "duplicate VAR" hint with line numbers', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'xi_dupvar_'));
    const src = join(dir, 'inspect.cpp');
    writeFileSync(src, [
        '#include <xi/xi.hpp>',
        'XI_SCRIPT_EXPORT',
        'void xi_inspect_entry(int frame) {',
        '    VAR(thing, frame);',
        '    VAR(other, frame + 1);',
        '    VAR(thing, frame + 2);',   // duplicate of `thing`
        '}',
        '',
    ].join('\n'));

    try {
        await withBackend(async (c) => {
            await c.nextText();
            c.send({ type: 'cmd', id: 1, name: 'compile_and_load', args: { path: slash(src) } });
            const r = await rsp(c, 1);
            assert.equal(r.ok, false, 'compile fails on duplicate VAR');
            const diags = (r.data && r.data.diagnostics) || [];
            const hinted = diags.find((d) => /duplicate VAR\(thing\)/.test(d.message || ''));
            assert.ok(hinted, 'a diagnostic carries the duplicate-VAR hint');
            assert.match(hinted.message, /lines? 4, 6/, 'hint lists the two VAR line numbers');
            assert.match(hinted.message, /EMIT\(thing\)/, 'hint suggests EMIT as the fix');
        });
    } finally {
        try { rmSync(dir, { recursive: true, force: true }); } catch {}
    }
});
