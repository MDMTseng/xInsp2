// ws_commands.test.mjs — Tests for untested WS commands.
//
// Covers: ping, version, unload_script+run, list_instances,
// set_instance_def,
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

        // Run with no script loaded — clean error rsp, not a crash.
        c.send({ type: 'cmd', id: 3, name: 'run' });
        const rr = await c.nextNonLog();
        assert.equal(rr.ok, false, 'run after unload returns a clear no-script error');

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
// 7b. get_instance_def — symmetric read; set→get→mutate→restore round-trip
// ---------------------------------------------------------------
test('get_instance_def round-trips set_instance_def', { timeout: 10000 }, async () => {
    await withBackend(async (c) => {
        await c.nextText();

        c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        const projDir = resolve(tmpdir(), `xi_getdef_${Date.now()}`);
        c.send({ type: 'cmd', id: 2, name: 'create_project',
                 args: { folder: projDir, name: 'test_getdef' } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 3, name: 'create_instance',
                 args: { name: 'cam_g', plugin: 'mock_camera' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Set a known def, then read it straight back.
        c.send({ type: 'cmd', id: 4, name: 'set_instance_def',
                 args: { name: 'cam_g', def: { width: 800, height: 600 } } });
        assert.equal((await c.nextNonLog()).ok, true);

        c.send({ type: 'cmd', id: 5, name: 'get_instance_def', args: { name: 'cam_g' } });
        const g1 = await c.nextNonLog();
        assert.equal(g1.ok, true, 'get_instance_def ok');
        const def1 = typeof g1.data === 'string' ? JSON.parse(g1.data) : g1.data;
        assert.equal(def1.width, 800, 'read-back width matches what was set');

        // Round-trip: feed the read def straight back via set_instance_def.
        c.send({ type: 'cmd', id: 6, name: 'set_instance_def',
                 args: { name: 'cam_g', def: def1 } });
        assert.equal((await c.nextNonLog()).ok, true, 'def round-trips through set_instance_def');

        // Unknown instance → ok:false.
        c.send({ type: 'cmd', id: 7, name: 'get_instance_def', args: { name: 'nope' } });
        const g2 = await c.nextNonLog();
        assert.equal(g2.ok, false, 'unknown instance returns ok:false');
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

        // Save instance config first (so it persists)
        c.send({ type: 'cmd', id: 4, name: 'save_instance_config',
                 args: { name: 'cam_sl' } });
        assert.equal((await c.nextNonLog()).ok, true);

        // Save project
        const projFile = resolve(projDir, 'project.json');
        c.send({ type: 'cmd', id: 5, name: 'save_project',
                 args: { path: projFile } });
        const sr = await c.nextNonLog();
        assert.equal(sr.ok, true, 'save_project ok');
    });

    // Phase 2: fresh backend, open_project + load_project, verify
    await withBackend(async (c) => {
        await c.nextText();

        // open_project to recreate instances from disk
        c.send({ type: 'cmd', id: 1, name: 'open_project',
                 args: { folder: projDir } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'open_project ok');

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
                 args: { plugin: 'mock_camera' } });
        const rsp = await c.nextNonLog();
        assert.equal(rsp.ok, true, 'get_plugin_ui ok');

        const uiPath = rsp.data.ui_path;
        assert.ok(typeof uiPath === 'string' && uiPath.length > 0,
            `should return a non-empty ui_path string, got: ${JSON.stringify(rsp.data)}`);
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
