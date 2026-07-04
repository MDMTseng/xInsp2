// ws_trigger_metadata.test.mjs — source-emitted event metadata rides the PACK
// (v12) end to end.
//
// v12 (THE CUT): xi::emit_record + current_trigger().meta() are gone — a
// source emits a sealed xi.pack@1 pack (new_pack()/emit()) whose ENTRIES are
// the event metadata ({command, recipe, seq}), and the inspect script reads
// them back with t.pack() (get_str/get_i64). This embeds a minimal pack-mode
// source plugin (cribbed from sdk/templates/expert), opens the project, starts
// TRIGGER-ONLY continuous mode, and asserts the metadata surfaced through the
// per-run verdict (xi::result → run_result) — proving the pack rode the bus
// correlated to its trigger, no FIFO, no Record meta doc.

import test from 'node:test';
import assert from 'node:assert/strict';
import { join, resolve } from 'node:path';
import { mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const slash = (s) => s.split('\\').join('/');

const SOURCE_CPP = `// meta_source — emits a metadata-only pack every 50 ms while running.
#include <xi/xi_thread.hpp>   // xi::spawn_worker (SEH-safe)
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
class MetaSource : public xi::Plugin {
    std::atomic<bool> running_{false};
    std::thread worker_;
public:
    using xi::Plugin::Plugin;
    ~MetaSource() override { stop_(); }
    std::string exchange(const std::string& cmd) override {
        if (cmd.find("\\"start\\"") != std::string::npos) start_();
        else if (cmd.find("\\"stop\\"") != std::string::npos) stop_();
        return std::string("{\\"running\\":") + (running_.load() ? "true" : "false") + "}";
    }
private:
    void start_() {
        if (running_.exchange(true)) return;
        worker_ = xi::spawn_worker(name() + "-src", [this] { loop_(); });
    }
    void stop_() {
        if (!running_.exchange(false)) return;
        if (worker_.joinable()) worker_.join();
    }
    void loop_() {
        long long seq = 0;
        while (running_.load()) {
            // Metadata rides the pack as ordinary entries (v12).
            xi::PackOut f = new_pack();
            f.str("command", "inspect_top");
            f.i64("recipe", 7);
            f.i64("seq", seq++);
            emit(std::move(f));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
};
XI_PLUGIN_IMPL(MetaSource)
`;

const SOURCE_JSON = JSON.stringify({
    name: 'meta_source', description: 'emits metadata-tagged packs (test fixture)',
    dll: 'meta_source.dll', factory: 'xi_plugin_create', has_ui: false,
}, null, 2);

const SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <xi/xi_result.hpp>
XI_INSPECT_ENTRY(t, frame) {
    if (!t.is_active()) return;                 // synthetic tick — nothing to read
    auto f = t.pack();
    if (!f) { xi::result(-1, "no pack"); return; }
    long long recipe = f.get_i64("recipe").value_or(-1);
    std::string cmd(f.get_str("command").value_or("none"));
    // code = recipe, msg = command@primary_source
    xi::result((int)recipe, cmd + "@" + t.primary_source());
}
`;

function makeProject(dir) {
    mkdirSync(join(dir, 'plugins', 'meta_source', 'src'), { recursive: true });
    mkdirSync(join(dir, 'instances', 'src'), { recursive: true });
    writeFileSync(join(dir, 'project.json'),
        JSON.stringify({ name: 'trigger_metadata', script: 'inspect.cpp', params: [], instances: [],
            plugins: { meta_source: { path: 'meta_source', compile: true } } }, null, 2));
    writeFileSync(join(dir, 'inspect.cpp'), SCRIPT);
    writeFileSync(join(dir, 'plugins', 'meta_source', 'plugin.json'), SOURCE_JSON);
    writeFileSync(join(dir, 'plugins', 'meta_source', 'src', 'plugin.cpp'), SOURCE_CPP);
    writeFileSync(join(dir, 'instances', 'src', 'instance.json'),
        JSON.stringify({ plugin: 'meta_source', config: {} }, null, 2));
}

async function rsp(c, id) {
    for (;;) { const m = await c.nextText(180000); if (m.type === 'rsp' && m.id === id) return m; }
}

test('source-emitted pack metadata reaches the script via t.pack()', { timeout: 240000 }, async () => {
    const dir = resolve(tmpdir(), `xi_trigmeta_${Date.now()}`);
    makeProject(dir);
    try {
        await withBackend(async (c) => {
            await c.nextText();  // hello

            // open_project: registers + cl-compiles meta_source, creates `src`.
            c.send({ type: 'cmd', id: 1, name: 'open_project', args: { folder: slash(dir) } });
            assert.equal((await rsp(c, 1)).ok, true, 'open_project ok');

            c.send({ type: 'cmd', id: 2, name: 'compile_and_load', args: { path: slash(join(dir, 'inspect.cpp')) } });
            assert.equal((await rsp(c, 2)).ok, true, 'compile ok');

            // Trigger-only continuous (fps<=0): the source is the only driver.
            c.send({ type: 'cmd', id: 3, name: 'start', args: { fps: 0 } });
            assert.equal((await rsp(c, 3)).ok, true, 'start ok');

            // Start the source's emit loop.
            c.send({ type: 'cmd', id: 4, name: 'exchange_instance',
                     args: { name: 'src', cmd: { command: 'start' } } });
            assert.equal((await rsp(c, 4)).ok, true, 'source started');

            // Collect run_results until one carries the metadata (or time out).
            let hit = null;
            const ok = await c.waitFor(() => {
                for (const m of c.drainText()) {
                    if (m.type !== 'event' || m.name !== 'run_result') continue;
                    if (m.data && m.data.code === 7 &&
                        String(m.data.msg || '').startsWith('inspect_top')) { hit = m.data; return true; }
                }
                return false;
            }, { timeoutMs: 20000, pollMs: 50 });

            c.send({ type: 'cmd', id: 5, name: 'exchange_instance',
                     args: { name: 'src', cmd: { command: 'stop' } } });
            await rsp(c, 5);
            c.send({ type: 'cmd', id: 6, name: 'stop' });
            await rsp(c, 6);

            assert.ok(ok, 'a run_result carried the pack metadata (command=inspect_top, recipe=7)');
            console.log('trigger metadata run_result:', JSON.stringify(hit));
            // recipe (7) already gated above; the emitting instance also rode through.
            assert.ok(String(hit.msg).includes('@src'), 'primary_source surfaced as "src"');
        });
    } finally {
        try { rmSync(dir, { recursive: true }); } catch {}
    }
});
