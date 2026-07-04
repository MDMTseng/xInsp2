// ws_buffer_replay.test.mjs — replay as plugin composition, pack plane (v12).
//
// The host doesn't own record/replay; a buffer plugin does. The inspect feeds
// each LIVE frame into the buffer via xi::use("buffer").process(pack); an
// exchange command re-emits buffered frames as fresh sealed packs
// (new_pack()/emit() — the v12 emit door; xi::emit_record is gone) so the
// script re-runs on them — the hot-param re-inspect loop. This drives that end
// to end and asserts a replayed inspection actually ran (a "replayed" pack
// entry surfaced through the run_result verdict).
//
// NB: exchange_instance `cmd` is passed as an OBJECT (a JSON string would be
// double-escaped on the wire and the plugin couldn't parse it).

import test from 'node:test';
import assert from 'node:assert/strict';
import { join, resolve } from 'node:path';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function rsp(c, id) { for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; } }

const BUFFER_CPP = `// buffer_replay — buffers live frames' payload; re-emits them on demand
// with a "replayed" marker entry (a sealed pack is immutable, so a replay is
// a NEW pack carrying the buffered payload + the marker).
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
class BufferReplay : public xi::Plugin {
    std::deque<long long> seqs_;
    mutable std::mutex mu_;
public:
    using xi::Plugin::Plugin;
    void process(xi::PackIn& in, xi::PackOut& out) override {
        std::lock_guard<std::mutex> lk(mu_);
        seqs_.push_back((long long)in.i64_or("seq", -1));
        while (seqs_.size() > 64) seqs_.pop_front();
        out.i64("buffered", (long long)seqs_.size());
    }
    std::string exchange(const std::string& cmd) override {
        if (cmd.find("\\"replay_last\\"") != std::string::npos) {
            int n = 1;
            auto pos = cmd.find("\\"n\\":");
            if (pos != std::string::npos) n = std::atoi(cmd.c_str() + pos + 4);
            std::vector<long long> picks;
            {
                std::lock_guard<std::mutex> lk(mu_);
                long long start = (long long)seqs_.size() - n;
                if (start < 0) start = 0;
                for (size_t i = (size_t)start; i < seqs_.size(); ++i) picks.push_back(seqs_[i]);
            }
            for (long long s : picks) {
                xi::PackOut f = new_pack();
                f.i64("seq", s);
                f.boolean("replayed", true);
                emit(std::move(f));        // re-emit -> the script re-runs on it
            }
            return "{\\"replayed\\":" + std::to_string(picks.size()) + "}";
        }
        std::lock_guard<std::mutex> lk(mu_);
        return "{\\"count\\":" + std::to_string(seqs_.size()) + "}";
    }
};
XI_PLUGIN_IMPL(BufferReplay)
XI_PLUGIN_PACK_DOOR(BufferReplay)
`;

const BUFFER_JSON = JSON.stringify({
    name: 'buffer_replay', description: 'pack buffer + replay-on-exchange (test fixture)',
    dll: 'buffer_replay.dll', factory: 'xi_plugin_create', has_ui: false,
}, null, 2);

// Live (timer-tick) runs feed a fresh seq-stamped pack into the buffer; a
// replayed trigger (pack carries replayed=true) surfaces through the verdict.
const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_script_pack.hpp>
#include <xi/xi_result.hpp>
XI_INSPECT_ENTRY(t, frame) {
    auto f = t.pack();
    if (f && f.get_bool("replayed").value_or(false)) {
        xi::result((int)f.get_i64("seq").value_or(-2) + 1, "replayed");
        return;
    }
    long long seq;
    {
        std::lock_guard<std::mutex> lk(xi::kv_mutex());
        seq = xi::kv().get_i64("seq", 0);
        xi::kv().set_i64("seq", seq + 1);
    }
    xi::ScriptPackBuilder b;
    b.add_i64("seq", seq);
    auto r = xi::use("buffer").process(b.seal());
    xi::result((int)r.get_i64("buffered").value_or(0), "live");
}
`;

function makeProject(dir) {
    mkdirSync(join(dir, 'plugins', 'buffer_replay', 'src'), { recursive: true });
    mkdirSync(join(dir, 'instances', 'buffer'), { recursive: true });
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'buffer_replay_demo', script: 'inspect.cpp', params: [], instances: [],
            plugins: { buffer_replay: { path: 'buffer_replay', compile: true } } }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'), SCRIPT);
    writeFileSync(join(dir, 'plugins', 'buffer_replay', 'plugin.json'), BUFFER_JSON);
    writeFileSync(join(dir, 'plugins', 'buffer_replay', 'src', 'plugin.cpp'), BUFFER_CPP);
    writeFileSync(join(dir, 'instances', 'buffer', 'instance.json'),
        JSON.stringify({ plugin: 'buffer_replay', config: {} }, null, 2));
}

test('buffer_replay captures live frames and re-emits them on exchange (replay loop)', { timeout: 240000 }, async () => {
    const dir = resolve(tmpdir(), `xi_bufreplay_${Date.now()}`);
    makeProject(dir);
    try {
        await withBackend(async (c) => {
            await c.nextText();

            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
            assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');
            c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
            assert.equal((await rsp(c, 2)).ok, true, 'compile ok');

            // Run live on the synthetic tick: the inspect feeds each frame into
            // the buffer.
            c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 15 } });
            assert.equal((await rsp(c, 3)).ok, true, 'start ok');
            await sleep(700);

            // The buffer should have captured frames.
            c.send({ type: 'cmd', id: 4, name: 'exchange_instance', args: { name: 'buffer', cmd: { command: 'get_count' } } });
            const d = (await rsp(c, 4)).data;
            const def = typeof d === 'string' ? JSON.parse(d || '{}') : (d || {});
            assert.ok(def.count > 0, `buffer captured live frames (count=${def.count})`);
            c.drainText();

            // Replay the last 2 buffered frames → 2 re-inspections with the
            // "replayed" marker riding the re-emitted pack.
            c.send({ type: 'cmd', id: 5, name: 'exchange_instance', args: { name: 'buffer', cmd: { command: 'replay_last', n: 2 } } });
            await rsp(c, 5);

            let replayed = 0;
            await c.waitFor(() => {
                for (const m of c.drainText()) {
                    if (m.type !== 'event' || m.name !== 'run_result') continue;
                    if (m.data && m.data.msg === 'replayed') replayed++;
                }
                return replayed >= 1;
            }, { timeoutMs: 8000, pollMs: 50 });

            c.send({ type: 'cmd', id: 6, name: 'stop' }); await rsp(c, 6);
            assert.ok(replayed >= 1, `a replayed inspection ran (msg "replayed"); got ${replayed}`);
            console.log(`buffer_replay: captured ${def.count}, replayed ${replayed}`);
        });
    } finally {
        try { rmSync(dir, { recursive: true }); } catch {}
    }
});
