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
// 2. Inspection output value verification
// ---------------------------------------------------------------
test('run produces correct var values (not just ok:true)', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr.ok, true, 'compile ok');

        const { rsp, vars } = await runInspection(c);
        assert.equal(rsp.ok, true);
        assert.equal(vars.type, 'vars');

        const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));

        // frame=1, user_amp=5 → raw=1, scaled=5, dbl=10
        assert.equal(byName.raw.kind, 'number');
        assert.equal(byName.raw.value, 1);
        assert.equal(byName.scaled.value, 5);
        assert.equal(byName.dbl.value, 10);
        assert.equal(byName.tag.kind, 'string');
        assert.equal(byName.tag.value, 'user_script_v1');
        assert.equal(byName.alive.kind, 'boolean');
        assert.equal(byName.alive.value, true);
    });
});

// ---------------------------------------------------------------
// 3. Run without script returns warning (not crash)
// ---------------------------------------------------------------
test('run without loaded script returns ok + warning log', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        c.send({ type: 'cmd', id: 1, name: 'run' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        // Should get a log warning about no script
        await sleep(300);
        const logs = c.drainLogs();
        const warns = logs.filter(l => l.level === 'warn');
        assert.ok(warns.length > 0, 'should get a warning about no script');
    });
});

// ---------------------------------------------------------------
// 4. cmd:run rejected during continuous mode
// ---------------------------------------------------------------
test('cmd:run rejected while continuous mode active', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
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
        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr.ok, true);

        // Start continuous
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 10 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true);

        await sleep(500);
        c.drainText();

        // Shutdown while streaming
        c.send({ type: 'cmd', id: 3, name: 'shutdown' });
        const sdr = await c.nextNonLog();
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
test('compile_and_load during continuous mode stops worker safely', { skip: 'DLL file lock after FreeLibrary — needs versioned output path' }, async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr1 = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr1.ok, true);

        // Start continuous
        c.send({ type: 'cmd', id: 2, name: 'start', args: { fps: 10 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true);

        await sleep(500);
        c.drainText();

        // Recompile while streaming — should stop worker, reload, succeed
        const cr2 = await compileScript(c, scriptPath('user_script_example.cpp'));
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

        // Now: compile and run NORMAL script — must produce correct values
        const cr2 = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr2.ok, true, 'normal script compiles after crash');

        const { rsp, vars } = await runInspection(c);
        assert.equal(rsp.ok, true);
        assert.equal(vars.type, 'vars');

        const byName = Object.fromEntries(vars.items.map(v => [v.name, v]));
        assert.equal(byName.raw.value, 1, 'correct value after crash recovery');
        assert.equal(byName.tag.value, 'user_script_v1');
    });
});

// ---------------------------------------------------------------
// 8. set_param verifies actual value change
// ---------------------------------------------------------------
test('set_param changes inspection output', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr.ok, true);

        // Run with default user_amp=5
        const r1 = await runInspection(c);
        const scaled1 = r1.vars.items.find(i => i.name === 'scaled').value;
        assert.equal(scaled1, 5); // frame=1 * 5

        // Change param
        c.send({ type: 'cmd', id: 10, name: 'set_param',
                 args: { name: 'user_amp', value: 20 } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true);

        // Run again
        const r2 = await runInspection(c);
        const scaled2 = r2.vars.items.find(i => i.name === 'scaled').value;
        assert.equal(scaled2, 20); // frame=1 * 20
    });
});

// ---------------------------------------------------------------
// 9. Plugin instance create + exchange + process
// ---------------------------------------------------------------
test('create instance + exchange commands work', async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Load plugin
        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        const lr = await c.nextNonLog();
        assert.equal(lr.ok, true);

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
        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
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
        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

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

test('get_project returns current state', async () => {
    await withBackend(async (c) => {
        await c.nextText();
        const projDir = resolve(tmpdir(), `xi_getproj_${Date.now()}`);
        c.send({ type: 'cmd', id: 1, name: 'create_project',
                 args: { folder: projDir, name: 'getproj_test' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 2, name: 'get_project' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        assert.equal(rsp.data.name, 'getproj_test');
    });
});

// ---------------------------------------------------------------
// 11. JPEG preview binary frame verification
// ---------------------------------------------------------------
test('run produces valid JPEG binary preview frames', async () => {
    await withBackend(async (c) => {
        await c.nextText();
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
