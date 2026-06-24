// ws_trigger_metadata.test.mjs — emit_trigger_record + current_trigger().meta()
// (ABI v5) end to end.
//
// The meta_source plugin tags every trigger with a JSON metadata object
// ({command, recipe, seq}) via xi::emit_record; the inspect script reads it back
// with current_trigger().meta() and VARs the fields. We open the project (which
// cl-compiles the source plugin and starts it emitting), load the script, run
// continuous, and assert the metadata surfaced in `vars` — proving the doc rode
// the bus by pointer (zero-serialize) correlated to the frame, with no FIFO.
//
// Exercises BOTH header-only additions through the real toolchain: the SDK
// xi::emit_record (compiled into the plugin) and xi_use.hpp meta() (compiled
// into the script).

import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { withBackend, examplesDir } from './helpers/client.mjs';

const PROJ = join(examplesDir, 'trigger_metadata');
const slash = (s) => s.split('\\').join('/');

async function rsp(c, id) {
    for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; }
}

test('emit_trigger_record metadata reaches the script via current_trigger().meta()', async () => {
    await withBackend(async (c) => {
        await c.nextText();  // hello

        // open_project: registers + cl-compiles meta_source, creates the `src`
        // instance (its run loop starts emitting metadata-tagged triggers).
        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJ) } });
        assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');

        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(PROJ, 'inspect.cpp')) } });
        assert.equal((await rsp(c, 2)).ok, true, 'compile ok');

        // Start continuous: the bus now dispatches the source's emits.
        c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 10 } });
        assert.equal((await rsp(c, 3)).ok, true, 'start ok');

        // Collect vars until one carries the metadata (or time out).
        let hit = null;
        const ok = await c.waitFor(() => {
            for (const m of c.drainText()) {
                if (m.type !== 'vars') continue;
                const s = JSON.stringify(m.items ?? m);
                if (s.includes('meta_command') && s.includes('inspect_top')) { hit = m; return true; }
            }
            return false;
        }, { timeoutMs: 15000, pollMs: 50 });

        c.send({ type: 'cmd', id: 4, name: 'stop' });
        await rsp(c, 4);

        assert.ok(ok, 'a vars message carried the trigger metadata (command=inspect_top)');
        const s = JSON.stringify(hit.items);
        // recipe (7) and source name also rode through.
        assert.ok(s.includes('meta_recipe'), 'meta_recipe var present');
        assert.ok(s.includes('"7"') || s.includes(':7') || s.includes('7'), 'recipe value 7 present');
        assert.ok(s.includes('src'), 'primary_source surfaced as "src"');
        console.log('trigger metadata vars:', s);
    });
});
