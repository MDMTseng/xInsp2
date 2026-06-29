// ws_buffer_replay.test.mjs — replay as plugin composition (ABI v6).
//
// The host no longer owns record/replay; a buffer_replay plugin does. The
// inspect feeds each LIVE frame into the buffer via xi::use("buffer").process();
// an exchange command re-emits a buffered record via emit_record so the script
// re-runs on it — the hot-param re-inspect loop. This drives that end to end and
// asserts a replayed inspection actually ran (replayed=true var).
//
// NB: exchange_instance `cmd` is passed as an OBJECT (a JSON string would be
// double-escaped on the wire and the plugin couldn't parse it).

import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { withBackend, examplesDir } from './helpers/client.mjs';

const PROJ = join(examplesDir, 'buffer_replay_demo');
const slash = (s) => s.split('\\').join('/');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function rsp(c, id) { for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; } }

test('buffer_replay captures live frames and re-emits them on exchange (replay loop)', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin (replay still runs; observed via vars)' }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJ) } });
        assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');
        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(PROJ, 'inspect.cpp')) } });
        assert.equal((await rsp(c, 2)).ok, true, 'compile ok');

        // Run live: pulse_src emits, the inspect feeds each frame into buffer_replay.
        c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 15 } });
        assert.equal((await rsp(c, 3)).ok, true, 'start ok');
        await sleep(700);

        // The buffer should have captured frames.
        c.send({ type: 'cmd', id: 4, name: 'exchange_instance', args: { name: 'buffer', cmd: { command: 'get_def' } } });
        const d = (await rsp(c, 4)).data;
        const def = typeof d === 'string' ? JSON.parse(d || '{}') : (d || {});
        assert.ok(def.count > 0, `buffer captured live frames (count=${def.count})`);
        c.drainText();

        // Replay the last 2 buffered frames → 2 re-inspections with replayed=true.
        c.send({ type: 'cmd', id: 5, name: 'exchange_instance', args: { name: 'buffer', cmd: { command: 'replay_last', n: 2 } } });
        await rsp(c, 5);

        let replayed = 0;
        await c.waitFor(() => {
            for (const m of c.drainText()) {
                if (m.type !== 'vars') continue;
                if (JSON.stringify(m.items).includes('"name":"replayed","kind":"boolean","value":true')) replayed++;
            }
            return replayed >= 1;
        }, { timeoutMs: 6000, pollMs: 50 });

        c.send({ type: 'cmd', id: 6, name: 'stop' }); await rsp(c, 6);
        assert.ok(replayed >= 1, `a replayed inspection ran (replayed=true var); got ${replayed}`);
        console.log(`buffer_replay: captured ${def.count}, replayed ${replayed}`);
    });
});
