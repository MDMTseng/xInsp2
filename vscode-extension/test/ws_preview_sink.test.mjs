// ws_preview_sink.test.mjs — validates the post-VAR output model.
//
// VAR + the core vars/preview wire were removed. A script surfaces output via
// xi::use("preview").process(Record{values + images}, tagged by pg_id). The
// preview_sink plugin:
//   - PUSHES each image LIVE as a binary frame (host_api emit_binary, ABI v8);
//   - skips re-compressing an identical image (content-hash dedup);
//   - serves the collapsed tab view (list_groups / get) + a pull still (get_image).
// This proves the script -> plugin -> WS-client path with no core vars frame.

import test from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { withBackend, examplesDir } from './helpers/client.mjs';

const PROJ = join(examplesDir, 'preview_sink_demo');
const slash = (s) => s.split('\\').join('/');
async function rsp(c, id) { for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; } }

// Parse a preview binary frame: 'XPV1' | u16 w | u16 h | u8 c | u8 codec | u8 pg_len | u8 key_len | pg | key | jpeg
function parseFrame(buf) {
    const magic = buf.toString('latin1', 0, 4);
    const w = buf.readUInt16LE(4), h = buf.readUInt16LE(6);
    const c = buf[8], codec = buf[9], pgLen = buf[10], keyLen = buf[11];
    let o = 12;
    const pg = buf.toString('utf8', o, o + pgLen); o += pgLen;
    const key = buf.toString('utf8', o, o + keyLen); o += keyLen;
    return { magic, w, h, c, codec, pg, key, jpegLen: buf.length - o };
}

test('preview_sink: multi-group output, live binary push, content-hash dedup', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(PROJ) } });
        assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');

        c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(PROJ, 'inspect.cpp')) } });
        assert.equal((await rsp(c, 2)).ok, true, 'compile ok (VAR stub still compiles)');

        c.send({ type: 'cmd', id: 3, name: 'run' });
        assert.equal((await rsp(c, 3)).ok, true, 'run ok');

        let nextId = 4;
        const exch = async (cmd) => {
            const id = nextId++;
            c.send({ type: 'cmd', id, name: 'exchange_instance', args: { name: 'preview', cmd } });
            const d = (await rsp(c, id)).data;
            return typeof d === 'string' ? JSON.parse(d || '{}') : (d || {});
        };

        // --- Live binary push: synth + thumb (bright) + inv (dark) = 3 frames ---
        await c.waitFor(() => c.binaryQueue.length >= 3, { timeoutMs: 10000, pollMs: 100 });
        const frames = c.binaryQueue.splice(0).map(parseFrame);
        console.log('binary frames:', frames.map(f => `${f.pg}/${f.key} ${f.w}x${f.h} jpeg=${f.jpegLen}`));
        assert.ok(frames.length >= 3, 'three image frames pushed live');
        assert.ok(frames.every(f => f.magic === 'XPV1' && f.codec === 1), 'all frames are XPV1/jpeg');
        const synth = frames.find(f => f.pg === 'bright' && f.key === 'synth');
        assert.ok(synth, 'bright/synth frame pushed');
        assert.deepEqual([synth.w, synth.h, synth.c], [8, 8, 1], 'frame header dims');
        assert.ok(synth.jpegLen > 0, 'frame carries a JPEG payload');
        assert.ok(frames.some(f => f.pg === 'dark' && f.key === 'inv'), 'dark/inv frame pushed');

        // --- Tabs from list_groups, with dedup counters ---
        let groups = {};
        for (let i = 0; i < 50; i++) {
            groups = await exch({ command: 'list_groups' });
            if ((groups.count || 0) >= 2) break;
            await new Promise(r => setTimeout(r, 100));
        }
        console.log('preview_sink list_groups:', JSON.stringify(groups));
        assert.equal(groups.count, 2, 'two preview groups');
        assert.ok(groups.groups.bright && groups.groups.dark, 'bright + dark present');
        // synth and thumb are the SAME buffer → encoded once; inv is distinct → 2 encodes total.
        assert.equal(groups.encodes, 2, 'two distinct images encoded');
        assert.ok(groups.dedup_hits >= 1, 'thumb reused synth\'s JPEG (dedup hit)');

        // --- Collapsed tab view: layout + values + line numbers, NO pixels ---
        const bright = await exch({ command: 'get', pg: 'bright' });
        assert.equal(bright.found, true, 'bright found');
        assert.equal(bright.image_count, 2, 'bright has synth + thumb');
        assert.ok(!('jpeg_b64' in bright), 'get (tab) carries no pixels');
        const bd = JSON.parse(bright.data);
        assert.equal(bd.score, 17); assert.equal(bd.gain, 7);
        assert.deepEqual(bd.$layout.map(e => [e.key, e.kind]),
            [['score', 'value'], ['gain', 'value'], ['synth', 'image'], ['thumb', 'image']], 'layout order + kinds');
        assert.ok(bd.$layout.every(e => typeof e.line === 'number' && e.line > 0), 'each entry stamps a source line');

        // --- Pull still on demand (expand): a JPEG data-URL payload ---
        const im = await exch({ command: 'get_image', pg: 'bright', key: 'synth' });
        assert.equal(im.found, true, 'get_image found');
        assert.deepEqual([im.w, im.h, im.channels], [8, 8, 1], 'dims round-trip');
        assert.ok(typeof im.jpeg_b64 === 'string' && im.jpeg_b64.length > 0, 'still as JPEG base64');
        assert.equal((await exch({ command: 'get_image', pg: 'bright', key: 'nope' })).found, false, 'missing → found:false');
    });
});
