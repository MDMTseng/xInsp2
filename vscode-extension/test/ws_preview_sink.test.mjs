// ws_preview_sink.test.mjs — validates the post-VAR output model.
//
// VAR + the core vars/preview wire were removed (branch refactor/remove-var-core).
// A script surfaces what it wants to view by pushing a Record into the
// preview_sink plugin: xi::use("preview").process(Record{score, gain, synth-img}).
// A client pulls the latest back via exchange_instance({"command":"get_latest"}).
// This proves data flows script -> plugin -> client with no core vars frame.

import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { withBackend, examplesDir } from './helpers/client.mjs';

const PROJ = join(examplesDir, 'preview_sink_demo');
const slash = (s) => s.split('\\').join('/');
async function rsp(c, id) { for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; } }

test('preview_sink: a script surfaces output via use().process, pulled back by exchange_instance', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJ) } });
        assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');

        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(PROJ, 'inspect.cpp')) } });
        assert.equal((await rsp(c, 2)).ok, true, 'compile ok (VAR stub still compiles)');

        // Single-shot run → inspect() pushes a Record into the preview_sink plugin.
        c.send({ type: 'cmd', id: 3, name: 'run' });
        assert.equal((await rsp(c, 3)).ok, true, 'run ok');

        // Pull the latest surfaced record back out of the plugin.
        c.send({ type: 'cmd', id: 4, name: 'exchange_instance',
                 args: { name: 'preview', cmd: { command: 'get_latest' } } });
        const d = (await rsp(c, 4)).data;
        const got = typeof d === 'string' ? JSON.parse(d || '{}') : (d || {});
        console.log('preview_sink get_latest:', JSON.stringify(got));

        assert.ok(got.seen >= 1, `plugin received at least one record (seen=${got.seen})`);
        assert.equal(got.image_count, 1, 'the synthetic image reached the plugin');
        // `data` is the script's surfaced data JSON (string-embedded).
        const data = typeof got.data === 'string' ? got.data : JSON.stringify(got.data);
        assert.ok(data.includes('score'), 'surfaced data carries `score`');
        assert.ok(data.includes('gain'),  'surfaced data carries `gain`');
    });
});
