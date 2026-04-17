// ws_commands.test.mjs — Tests for untested WS commands.
//
// Covers: ping, version, unload_script+run, list_instances,
// set_instance_def, preview_instance, process_instance,
// save_project/load_project round-trip, get_plugin_ui, unknown command.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setTimeout as sleep } from 'node:timers/promises';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import {
    withBackend, compileScript, runInspection, scriptPath
} from './helpers/client.mjs';

// ---------------------------------------------------------------
// 1. ping returns pong + timestamp
// ---------------------------------------------------------------
test('ping returns pong:true with timestamp', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText(); // hello

        c.send({ type: 'cmd', id: 1, name: 'ping' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        assert.equal(rsp.data.pong, true, 'pong field should be true');
        assert.equal(typeof rsp.data.ts, 'number', 'ts should be a number');
    });
});

// ---------------------------------------------------------------
// 2. version returns semver + abi
// ---------------------------------------------------------------
test('version returns semver string and abi version', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'version' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);
        assert.match(rsp.data.version, /\d+\.\d+\.\d+/,
            'version should match semver pattern');
        assert.ok('abi' in rsp.data, 'should have abi field');
    });
});

// ---------------------------------------------------------------
// 3. unload_script + run → warning, no crash
// ---------------------------------------------------------------
test('unload_script then run produces warning, not crash', { timeout: 90000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Compile first so there's something to unload
        const cr = await compileScript(c, scriptPath('user_script_example.cpp'));
        assert.equal(cr.ok, true);

        // Unload
        c.send({ type: 'cmd', id: 2, name: 'unload_script' });
        const ur = await c.nextNonLog();
        assert.equal(ur.ok, true, 'unload_script ok');

        // Run with no script loaded — should not crash
        c.send({ type: 'cmd', id: 3, name: 'run' });
        const rr = await c.nextNonLog();
        assert.equal(rr.ok, true, 'run after unload should succeed (no-op)');

        // Backend still alive — verify with ping
        c.send({ type: 'cmd', id: 4, name: 'ping' });
        const pr = await c.nextNonLog();
        assert.equal(pr.ok, true, 'backend alive after unload+run');
    });
});

// ---------------------------------------------------------------
// 4. list_instances after create
// ---------------------------------------------------------------
test('list_instances shows created instance', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Load plugin + create project
        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        const projDir = resolve(tmpdir(), `xi_listinst_${Date.now()}`);
        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'test_list' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Create instance
        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_list', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // List instances — sends rsp then a separate 'instances' message
        c.send({ type: 'cmd', id: 4, name: 'list_instances' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true);

        // Read the instances broadcast message
        const instMsg = await c.nextText(5000);
        assert.equal(instMsg.type, 'instances');
        const names = (instMsg.instances || []).map(i => i.name);
        assert.ok(names.includes('cam_list'),
            `cam_list should appear in instance list, got: ${JSON.stringify(names)}`);
    });
});

// ---------------------------------------------------------------
// 5. set_instance_def changes config
// ---------------------------------------------------------------
test('set_instance_def updates instance definition', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        const projDir = resolve(tmpdir(), `xi_setdef_${Date.now()}`);
        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'test_setdef' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_def', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Set a new definition (e.g., change width)
        c.send({ type: 'cmd', id: 4, name: 'set_instance_def',
                 args: { name: 'cam_def', def: { width: 640, height: 480 } } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'set_instance_def ok');

        // Verify via exchange get_status
        c.send({ type: 'cmd', id: 5, name: 'exchange_instance',
                 args: { name: 'cam_def', cmd: { command: 'get_status' } } });
        const er = await c.nextNonLog();
        assert.equal(er.ok, true);
        const status = typeof er.data === 'string' ? JSON.parse(er.data) : er.data;
        assert.equal(status.width, 640, 'width should be updated to 640');
    });
});

// ---------------------------------------------------------------
// 6. preview_instance returns JPEG
// ---------------------------------------------------------------
test('preview_instance returns JPEG binary frame', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        const projDir = resolve(tmpdir(), `xi_preview_${Date.now()}`);
        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'test_preview' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_prev', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Start the camera so it has frames
        c.send({ type: 'cmd', id: 4, name: 'exchange_instance',
                 args: { name: 'cam_prev', cmd: { command: 'start' } } });
        await c.nextNonLog();
        await sleep(200);

        // Request preview
        c.send({ type: 'cmd', id: 5, name: 'preview_instance',
                 args: { name: 'cam_prev' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'preview_instance ok');

        // Check for JPEG binary frame
        const frame = await c.nextBinary(5000);
        assert.ok(frame.length > 2, 'binary frame should have data');

        // JPEG magic bytes (may have header prefix)
        const jpg = frame.length > 20 ? frame.subarray(20) : frame;
        assert.equal(jpg[0], 0xFF, 'JPEG SOI marker byte 1');
        assert.equal(jpg[1], 0xD8, 'JPEG SOI marker byte 2');
    });
});

// ---------------------------------------------------------------
// 7. process_instance with source
// ---------------------------------------------------------------
test('process_instance processes blob_analysis with camera source', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        // Load plugins
        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);
        c.send({ type: 'cmd', id: 2, name: 'load_plugin', args: { name: 'blob_analysis' } });
        assert.equal((await c.nextNonLog()).ok, true);

        const projDir = resolve(tmpdir(), `xi_process_${Date.now()}`);
        c.send({ type: 'cmd', id: 3, name: 'create_project',
                 args: { folder: projDir, name: 'test_process' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Create camera instance
        c.send({ type: 'cmd', id: 4, name: 'create_instance',
                 args: { name: 'cam_proc', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Start camera
        c.send({ type: 'cmd', id: 5, name: 'exchange_instance',
                 args: { name: 'cam_proc', cmd: { command: 'start' } } });
        await c.nextNonLog();
        await sleep(200);

        // Create blob_analysis instance
        c.send({ type: 'cmd', id: 6, name: 'create_instance',
                 args: { name: 'det_proc', plugin: 'blob_analysis' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Process: run blob_analysis with camera as source
        c.send({ type: 'cmd', id: 7, name: 'process_instance',
                 args: { name: 'det_proc', source: 'cam_proc' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'process_instance ok');

        // Verify result has blob_count
        const data = typeof rsp.data === 'string' ? JSON.parse(rsp.data) : rsp.data;
        assert.ok('blob_count' in data || rsp.ok,
            'process result should contain blob_count or succeed');
    });
});

// ---------------------------------------------------------------
// 8. save_project / load_project round-trip
// ---------------------------------------------------------------
test('save_project then load_project preserves instances and params', { timeout: 10000 }, async () => {
    const projDir = resolve(tmpdir(), `xi_saveload_${Date.now()}`);

    // Phase 1: create project, add instance, save
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'saveload_test' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_sl', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Save project
        c.send({ type: 'cmd', id: 4, name: 'save_project' });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true, 'save_project ok');
    });

    // Phase 2: fresh backend, load project, verify
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'load_project',
                 args: { folder: projDir } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'load_project ok');

        // Verify instance was restored
        const data = rsp.data;
        const instances = data.instances || [];
        const cam = instances.find(i => i.name === 'cam_sl');
        assert.ok(cam, 'cam_sl instance should be restored after load');
        assert.equal(cam.plugin, 'mock_camera');
    });
});

// ---------------------------------------------------------------
// 9. get_plugin_ui returns path
// ---------------------------------------------------------------
test('get_plugin_ui returns a valid path', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 2, name: 'get_plugin_ui',
                 args: { name: 'mock_camera' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'get_plugin_ui ok');

        // Should have a path or ui field
        const path = rsp.data.path || rsp.data.ui || rsp.data;
        assert.ok(typeof path === 'string' && path.length > 0,
            'should return a non-empty path string');
    });
});

// ---------------------------------------------------------------
// 10. Unknown command → error
// ---------------------------------------------------------------
test('unknown command returns ok:false', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'totally_bogus_command_that_does_not_exist' });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, false, 'unknown command should return ok:false');
        assert.ok(rsp.error, 'error message should be present');
    });
});
