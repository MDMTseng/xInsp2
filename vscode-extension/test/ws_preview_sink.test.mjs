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

        // Single-shot run → inspect() pushes a Record into TWO preview groups.
        c.send({ type: 'cmd', id: 3, name: 'run' });
        assert.equal((await rsp(c, 3)).ok, true, 'run ok');

        let nextId = 4;
        const exch = async (cmd) => {
            const id = nextId++;
            c.send({ type: 'cmd', id, name: 'exchange_instance', args: { name: 'preview', cmd } });
            const d = (await rsp(c, id)).data;
            return typeof d === 'string' ? JSON.parse(d || '{}') : (d || {});
        };

        // cmd:run runs the inspect on a detached thread, so the run rsp can land
        // before both pv.process() calls do. Poll list_groups until both groups
        // appear (the UI would do the same — or subscribe to a push).
        let groups = {};
        for (let i = 0; i < 50; i++) {
            groups = await exch({ command: 'list_groups' });
            if ((groups.count || 0) >= 2) break;
            await new Promise(r => setTimeout(r, 100));
        }
        console.log('preview_sink list_groups:', JSON.stringify(groups));
        assert.equal(groups.count, 2, 'two preview groups exist');
        assert.ok(groups.groups.bright, 'group "bright" present');
        assert.ok(groups.groups.dark,   'group "dark" present');

        // Pull each group independently (what a tab switch does).
        const bright = await exch({ command: 'get', pg: 'bright' });
        console.log('preview_sink get bright:', JSON.stringify(bright));
        assert.equal(bright.found, true, 'bright group found');
        assert.equal(bright.image_count, 1, 'bright carries its image');

        // `data` is the script's surfaced record (string-embedded JSON). pvar() built
        // it: score(value), gain(value), synth(image) — the $layout array preserves
        // that order + tags so the UI renders top-to-bottom.
        const bd = JSON.parse(typeof bright.data === 'string' ? bright.data : JSON.stringify(bright.data));
        assert.equal(bd.score, 17, 'score value present');
        assert.equal(bd.gain, 7, 'gain value present');
        assert.deepEqual(bd.$layout, [
            { key: 'score', kind: 'value' },
            { key: 'gain',  kind: 'value' },
            { key: 'synth', kind: 'image' },
        ], '$layout preserves pvar order + tags the image');

        const dark = await exch({ command: 'get', pg: 'dark' });
        assert.equal(dark.found, true, 'dark group found');
        assert.equal(dark.image_count, 1, 'dark carries its image');

        // A missing group reports not-found (not an error).
        const none = await exch({ command: 'get', pg: 'nope' });
        assert.equal(none.found, false, 'unknown group → found:false');
    });
});
