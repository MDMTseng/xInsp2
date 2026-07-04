// Comprehensive test — covers the top 10 gaps from TestAudit.md.
//
// Each test uses the shared client helper. Tests are self-contained
// and can run independently.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import {
    withBackend, compileScript, runInspection, scriptPath
} from './helpers/client.mjs';

// In continuous mode the worker streams `vars` frames that can arrive
// before a command's `rsp` ack. Loop until we get the rsp.
async function ackRsp(c) {
    for (;;) { const m = await c.nextNonLog(); if (m.type === 'rsp') return m; }
}

// Minimal inline script (the old examples/user_script_example.cpp was retired
// with the VAR removal). Two script params so list_params has something to
// list; the verdict rides run_result (xi::result), not the removed vars frame.
const INLINE_SCRIPT = `#include <xi/xi.hpp>
#include <xi/xi_result.hpp>
xi::Param<int> user_amp{"user_amp", 5, {0, 100}};
xi::Param<int> user_bias{"user_bias", 0, {0, 100}};
XI_SCRIPT_EXPORT
void xi_inspect_entry(int frame) {
    xi::result(frame * (int)user_amp + (int)user_bias, "inline_script_v1");
}
`;
let _inlineScriptPath = null;
function inlineScript() {
    if (_inlineScriptPath) return _inlineScriptPath;
    const dir = resolve(tmpdir(), `xi_comprehensive_${process.pid}`);
    mkdirSync(dir, { recursive: true });
    _inlineScriptPath = resolve(dir, 'inline_script.cpp').split('\\').join('/');
    writeFileSync(_inlineScriptPath, INLINE_SCRIPT);
    return _inlineScriptPath;
}

// ---------------------------------------------------------------
// 1. Compile failure returns error (not silent)
// ---------------------------------------------------------------
test('compile failure returns ok:false with error message', async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello
        c.send({ type: 'cmd', id: 1, name: 'compile_and_load',
                 args: { path: 'C:/nonexistent/bad_script.cpp' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, false, 'compile should fail');
        assert.ok(rsp.error, 'error message should be present');
    });
});

// ---------------------------------------------------------------
// 2. Inspection output value verification (via run_result — the vars
//    frame was removed; the script's verdict is the observable value)
// ---------------------------------------------------------------
test('run produces the correct verdict values (not just ok:true)', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, inlineScript());
        assert.equal(cr.ok, true, 'compile ok');

        c.send({ type: 'cmd', id: 50, name: 'run' });
        // Collect the run_result event (nextText — nextNonLog filters events).
        let result = null;
        for (;;) {
            const m = await c.nextText();
            if (m.type === 'event' && m.name === 'run_result') { result = m.data; break; }
        }
        // frame=1, user_amp=5, user_bias=0 → code 5; msg is the script tag.
        assert.equal(result.code, 5, 'verdict code = frame * user_amp');
        assert.equal(result.msg, 'inline_script_v1', 'verdict msg from xi::result');
    });
});

// ---------------------------------------------------------------
// 3. Run without script returns warning (not crash)
// ---------------------------------------------------------------
test('run without loaded script returns a clean no-script error', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'run' });
        const rsp = await c.nextNonLog();
        // No script loaded → clear error rsp (not a crash, connection stays alive).
        assert.equal(rsp.ok, false);
        assert.match(String(rsp.error || ''), /no script/i);
    });
});

// ---------------------------------------------------------------
// 4. cmd:run rejected during continuous mode
// ---------------------------------------------------------------
test('cmd:run rejected while continuous mode active', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, inlineScript());
        assert.equal(cr.ok, true);

        // Start continuous
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 5 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true);

        await sleep(500);
        c.drainText(); // clear any vars messages from continuous mode

        // Try to run — should be rejected
        c.send({ type: 'cmd', id: 3, name: 'run' });
        const rr = await c.nextNonLog();
        assert.equal(rr.type, 'rsp', 'should get rsp not vars');
        assert.equal(rr.ok, false, 'run should be rejected during continuous');
        assert.ok(rr.error.includes('continuous'), 'error mentions continuous mode');

        // Stop
        c.send({ type: 'cmd', id: 4, name: 'stop' });
        const stopr = await c.nextNonLog();
        assert.equal(stopr.ok, true);
    });
});

// ---------------------------------------------------------------
// 5. Shutdown during continuous mode — clean exit
// ---------------------------------------------------------------
test('shutdown during continuous mode exits cleanly', async () => {
    await withBackend(async (c, child) => {
        await c.nextText();
        const cr = await compileScript(c, inlineScript());
        assert.equal(cr.ok, true);

        // Start continuous
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 10 } });
        const sr = await ackRsp(c);
        assert.equal(sr.ok, true);

        await sleep(500);
        c.drainText();

        // Shutdown while streaming
        c.send({ type: 'cmd', id: 3, name: 'shutdown' });
        const sdr = await ackRsp(c);
        assert.equal(sdr.ok, true);

        // Wait for clean exit
        for (let i = 0; i < 30; ++i) {
            if (child.exitCode !== null) break;
            await sleep(100);
        }
        assert.equal(child.exitCode, 0, 'backend should exit with code 0');
    });
});

// ---------------------------------------------------------------
// 6. Hot-reload during continuous mode — stops and restarts safely
// ---------------------------------------------------------------
test('compile_and_load during continuous mode stops worker safely', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr1 = await compileScript(c, inlineScript());
        assert.equal(cr1.ok, true);

        // Start continuous
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 10 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true);

        await sleep(500);
        c.drainText();

        // Recompile while streaming — should stop worker, reload, succeed
        const cr2 = await compileScript(c, inlineScript());
        assert.equal(cr2.ok, true, 'hot-reload during continuous should succeed');

        // Backend still alive — ping works
        c.send({ type: 'cmd', id: 10, name: 'ping' });
        const pong = await c.nextNonLog();
        assert.equal(pong.ok, true);
        assert.equal(pong.data.pong, true);
    });
});

// ---------------------------------------------------------------
// 7. SEH crash recovery + correct output after
// ---------------------------------------------------------------
test('crash recovery: normal script produces correct output after SEH crash', async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // First: compile and run crash script
        const crashPath = scriptPath('crash_tests/null_deref.cpp');
        const cr1 = await compileScript(c, crashPath);
        assert.equal(cr1.ok, true, 'crash script compiles');

        c.send({ type: 'cmd', id: 2, name: 'run' });
        await c.nextNonLog(); // rsp
        await sleep(500);

        // Verify crash was caught (error log)
        const logs = c.drainLogs();
        const errors = logs.filter(l => l.level === 'error');
        assert.ok(errors.some(e => e.msg.includes('ACCESS_VIOLATION')),
            'should log ACCESS_VIOLATION');

        // Now: compile and run NORMAL script — must produce the correct verdict
        // (observed via run_result; the vars frame was removed with VAR).
        const cr2 = await compileScript(c, inlineScript());
        assert.equal(cr2.ok, true, 'normal script compiles after crash');

        c.send({ type: 'cmd', id: 3, name: 'run' });
        let result = null;
        for (;;) {
            const m = await c.nextText();
            if (m.type === 'event' && m.name === 'run_result') { result = m.data; break; }
        }
        assert.equal(result.code, 5, 'correct verdict after crash recovery (frame=1 * user_amp=5)');
        assert.equal(result.msg, 'inline_script_v1');
    });
});

// ---------------------------------------------------------------
// 8. set_param verifies actual value change
// ---------------------------------------------------------------
test('set_param changes inspection output', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, inlineScript());
        assert.equal(cr.ok, true);

        // The verdict rides the run_result event: code = frame * user_amp.
        const runCode = async (id) => {
            c.send({ type: 'cmd', id, name: 'run' });
            for (;;) {
                const m = await c.nextText();
                if (m.type === 'event' && m.name === 'run_result') return m.data.code;
            }
        };

        // Run with default user_amp=5 (cmd:run always passes frame=1 → 5)
        assert.equal(await runCode(9), 5);

        // Change param
        c.send({ type: 'cmd', id: 10, name: 'set_param',
                 args: { name: 'user_amp', value: 20 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true);

        // Run again (frame=1 → 20)
        assert.equal(await runCode(12), 20);

        // %g regression: a big integer must parse fully, not truncate. Before the fix
        // 1000000 -> "%g" -> "1e+06" -> std::stoll stops at 'e' -> the param was set to
        // 1. user_amp clamps to its max (100), so a correct parse shows frame*100; the
        // truncated value would show frame*1.
        c.send({ type: 'cmd', id: 11, name: 'set_param',
                 args: { name: 'user_amp', value: 1000000 } });
        assert.equal((await c.nextNonLog()).ok, true);
        assert.equal(await runCode(13), 100,
            'big int parsed fully (clamped to 100), not truncated to 1');
    });
});

// ---------------------------------------------------------------
// 9. Plugin instance create + exchange + process
// ---------------------------------------------------------------
test('create instance + exchange commands work', async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // v12: `load_plugin` retired — create_instance loads the plugin itself.

        // Create project (required for create_instance)
        const projDir = resolve(tmpdir(), `xi_test_${Date.now()}`);
        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'test' } });
        const pr = await c.nextNonLog();
        assert.equal(pr.ok, true);

        // Create instance
        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_test', plugin: 'mock_camera' } });
        const ir = await c.nextNonLog();
        assert.equal(ir.ok, true);

        // Exchange: get_status
        c.send({ type: 'cmd', id: 4, name: 'exchange_instance',
                 args: { name: 'cam_test', cmd: { command: 'get_status' } } });
        const er = await c.nextNonLog();
        assert.equal(er.ok, true);
        const status = typeof er.data === 'string' ? JSON.parse(er.data) : er.data;
        assert.ok('width' in status, 'status has width');
        assert.ok('fps' in status, 'status has fps');

        // Exchange: set_fps
        c.send({ type: 'cmd', id: 5, name: 'exchange_instance',
                 args: { name: 'cam_test', cmd: { command: 'set_fps', value: 25 } } });
        const fr = await c.nextNonLog();
        assert.equal(fr.ok, true);
        const updated = typeof fr.data === 'string' ? JSON.parse(fr.data) : fr.data;
        assert.equal(updated.fps, 25);
    });
});

// ---------------------------------------------------------------
// 10. Untested commands: list_params, list_instances, open_project
// ---------------------------------------------------------------
test('list_params returns params from loaded script', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, inlineScript());
        assert.equal(cr.ok, true);

        c.send({ type: 'cmd', id: 2, name: 'list_params' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);

        // Should get an instances message with params
        const inst = await c.nextNonLog();
        assert.equal(inst.type, 'instances');
        assert.ok(Array.isArray(inst.params), 'params is array');
        const names = inst.params.map(p => p.name);
        assert.ok(names.includes('user_amp'), 'has user_amp');
        assert.ok(names.includes('user_bias'), 'has user_bias');
    });
});

test('open_project restores instances', async () => {
    const projDir = resolve(tmpdir(), `xi_open_test_${Date.now()}`);

    // Phase 1: create project with an instance
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'open_test' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_saved', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Save instance config
        c.send({ type: 'cmd', id: 4, name: 'save_instance_config',
                 args: { name: 'cam_saved' } });
        assert.equal((await c.nextNonLog()).ok, true);
    });

    // Phase 2: fresh backend, open the saved project
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'open_project',
                 args: { folder: projDir } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'open_project ok');

        // Verify the instance was restored
        const data = rsp.data;
        const instances = data.instances || [];
        const cam = instances.find(i => i.name === 'cam_saved');
        assert.ok(cam, 'cam_saved instance restored');
        assert.equal(cam.plugin, 'mock_camera');
    });
});

// ("get_project returns current state" was DELETED at THE CUT: `get_project`
// is one of the five retired commands. Project-model reads are covered by the
// open_project rsp payload (test above) and list_instances.)

// ---------------------------------------------------------------
// 11. JPEG preview binary frame verification
// ---------------------------------------------------------------
test('run produces valid JPEG binary preview frames', { skip: 'vars/preview/subscribe removed with VAR (branch refactor/remove-var-core) — pending preview plugin' }, async () => {
    await withBackend(async (c) => {
        await c.nextText();
        // Previews are off by default — opt into all for this headless check.
        c.send({ type: 'cmd', id: 9, name: 'subscribe', args: { all: true } });
        await c.nextNonLog();
        const cr = await compileScript(c, scriptPath('defect_detection.cpp'));
        assert.equal(cr.ok, true);

        c.send({ type: 'cmd', id: 2, name: 'run' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);

        const vars = await c.nextNonLog();
        const images = vars.items.filter(i => i.kind === 'image');
        assert.ok(images.length >= 3, `need >=3 images, got ${images.length}`);

        // Read one binary frame
        const frame = await c.nextBinary();
        assert.ok(frame.length > 20);

        // Check JPEG magic bytes
        const jpg = frame.subarray(20);
        assert.equal(jpg[0], 0xFF);
        assert.equal(jpg[1], 0xD8);
        assert.equal(jpg[jpg.length - 2], 0xFF);
        assert.equal(jpg[jpg.length - 1], 0xD9);
    });
});
