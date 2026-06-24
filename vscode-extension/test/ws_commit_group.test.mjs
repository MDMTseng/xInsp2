// ws_commit_group.test.mjs — orchestrator config-swap (tasks #66–#69).
//
// Validates the prepare/commit + drain-barrier design
// (docs/roadmap/config-bundles-and-orchestration.md) against the reference plugin
// config_swap_probe, which implements the first-class ABI v7 double-slot
// prepare()/commit() (XI_PLUGIN_STAGED):
//
//   1. prepare_instance stages a new config WITHOUT touching the live slot — the
//      active value stays put, the staged value is parked.
//   2. cmd:commit_group flips a GROUP of instances from staged → active in one
//      host drain-barrier (quiesce dispatch → commit() all → resume).
//   3. A plain run after start still works (backend resumed, stays alive).
//
// What this DOESN'T prove (and isn't trying to): the under-load atomicity of the
// barrier — that the swap lands in a no-process window — is the host's job and
// reuses the same quiesce primitive recompile/rebuild already rely on. This test
// nails the new wiring: staging semantics + group commit + resume.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { resolve } from 'node:path';
import { tmpdir } from 'node:os';
import { withBackend } from './helpers/client.mjs';

const parse = (d) => (typeof d === 'string' ? JSON.parse(d) : d);

async function setup(c) {
    await c.nextText(); // hello
    c.send({ type: 'cmd', id: 1, name: 'load_plugin', args: { name: 'config_swap_probe' } });
    assert.equal((await c.nextNonLog()).ok, true, 'load_plugin config_swap_probe');

    const projDir = resolve(tmpdir(), `xi_commitgrp_${Date.now()}`);
    c.send({ type: 'cmd', id: 2, name: 'create_project', args: { folder: projDir, name: 'cg' } });
    assert.equal((await c.nextNonLog()).ok, true, 'create_project');

    for (const [i, nm] of [['3', 'sw_a'], ['4', 'sw_b']]) {
        c.send({ type: 'cmd', id: +i, name: 'create_instance',
                 args: { name: nm, plugin: 'config_swap_probe' } });
        assert.equal((await c.nextNonLog()).ok, true, `create_instance ${nm}`);
    }
}

async function status(c, id, name) {
    c.send({ type: 'cmd', id, name: 'exchange_instance',
             args: { name, cmd: { command: 'get_status' } } });
    const r = await c.nextNonLog();
    assert.equal(r.ok, true, `get_status ${name}`);
    return parse(r.data);
}

test('prepare stages without swapping; commit_group flips the whole group', { timeout: 30000 }, async () => {
    await withBackend(async (c) => {
        await setup(c);

        // Set both instances live at value 1 (tier-1 immediate path).
        for (const [i, nm] of [['10', 'sw_a'], ['11', 'sw_b']]) {
            c.send({ type: 'cmd', id: +i, name: 'set_instance_def',
                     args: { name: nm, def: { value: 1 } } });
            assert.equal((await c.nextNonLog()).ok, true, `set_def ${nm}=1`);
        }
        assert.equal((await status(c, 12, 'sw_a')).active, 1, 'sw_a active=1');
        assert.equal((await status(c, 13, 'sw_b')).active, 1, 'sw_b active=1');

        // prepare both to value 2 — live slot must NOT move yet (first-class verb).
        for (const [i, nm] of [['20', 'sw_a'], ['21', 'sw_b']]) {
            c.send({ type: 'cmd', id: +i, name: 'prepare_instance',
                     args: { name: nm, def: { value: 2 } } });
            assert.equal((await c.nextNonLog()).ok, true, `prepare ${nm}=2`);
        }
        const a1 = await status(c, 22, 'sw_a');
        assert.equal(a1.active, 1, 'sw_a still live on 1 after prepare');
        assert.equal(a1.staged, true, 'sw_a has a staged resource');
        assert.equal(a1.staged_value, 2, 'sw_a staged value is 2');
        assert.equal((await status(c, 23, 'sw_b')).active, 1, 'sw_b still live on 1 after prepare');

        // commit_group: drain-barrier flips BOTH at once.
        c.send({ type: 'cmd', id: 30, name: 'commit_group',
                 args: { instances: ['sw_a', 'sw_b'] } });
        const cg = await c.nextNonLog();
        assert.equal(cg.ok, true, 'commit_group ok');
        const cgd = parse(cg.data);
        assert.equal(cgd.results.length, 2, 'two results');
        assert.ok(cgd.results.every(r => r.ok), 'every target committed');

        // Both instances are now live on 2, staging cleared.
        const a2 = await status(c, 31, 'sw_a');
        assert.equal(a2.active, 2, 'sw_a live on 2 after commit_group');
        assert.equal(a2.staged, false, 'sw_a staging cleared');
        assert.equal((await status(c, 32, 'sw_b')).active, 2, 'sw_b live on 2 after commit_group');
    });
});

test('commit_group while continuous: backend resumes and stays alive', { timeout: 30000 }, async () => {
    await withBackend(async (c) => {
        await setup(c);

        // Arm continuous mode (free-running timer; no source needed for this check).
        c.send({ type: 'cmd', id: 40, name: 'start', args: { fps: 5 } });
        await c.nextNonLog();

        c.send({ type: 'cmd', id: 41, name: 'prepare_instance',
                 args: { name: 'sw_a', def: { value: 7 } } });
        assert.equal((await c.nextNonLog()).ok, true, 'prepare sw_a=7');

        c.send({ type: 'cmd', id: 42, name: 'commit_group', args: { instances: ['sw_a'] } });
        assert.equal((await c.nextNonLog()).ok, true, 'commit_group ok under continuous');

        assert.equal((await status(c, 43, 'sw_a')).active, 7, 'sw_a live on 7');

        // Backend still responsive (dispatch resumed, not wedged).
        c.send({ type: 'cmd', id: 44, name: 'ping' });
        assert.equal((await c.nextNonLog()).ok, true, 'ping ok after commit_group');
    });
});

test('get_state tracks created → active across set_def and commit_group', { timeout: 30000 }, async () => {
    await withBackend(async (c) => {
        await setup(c);

        const getState = async (id, name) => {
            c.send({ type: 'cmd', id, name: 'get_state', args: { name } });
            const r = await c.nextNonLog();
            return r.ok ? parse(r.data) : { _err: r.error };
        };

        // Freshly created, no host-visible transition yet → "created".
        assert.equal((await getState(60, 'sw_a')).state, 'created', 'sw_a created');

        // set_instance_def → active.
        c.send({ type: 'cmd', id: 61, name: 'set_instance_def',
                 args: { name: 'sw_a', def: { value: 1 } } });
        assert.equal((await c.nextNonLog()).ok, true);
        assert.equal((await getState(62, 'sw_a')).state, 'active', 'sw_a active after set_def');

        // commit_group keeps it active (and the result carries last_error="").
        c.send({ type: 'cmd', id: 63, name: 'prepare_instance',
                 args: { name: 'sw_a', def: { value: 9 } } });
        await c.nextNonLog();
        c.send({ type: 'cmd', id: 64, name: 'commit_group', args: { instances: ['sw_a'] } });
        assert.equal((await c.nextNonLog()).ok, true);
        const st = await getState(65, 'sw_a');
        assert.equal(st.state, 'active', 'sw_a active after commit_group');
        assert.equal(st.last_error, '', 'no error on a clean commit');

        // Unknown instance → ok:false.
        assert.ok((await getState(66, 'ghost'))._err, 'unknown instance reports not-found');
    });
});

test('commit_group addresses a group by plugin-type selector', { timeout: 30000 }, async () => {
    await withBackend(async (c) => {
        await setup(c);

        // Stage both, then commit by the plugin-type selector (no explicit list).
        for (const [i, nm] of [['70', 'sw_a'], ['71', 'sw_b']]) {
            c.send({ type: 'cmd', id: +i, name: 'prepare_instance',
                     args: { name: nm, def: { value: 5 } } });
            assert.equal((await c.nextNonLog()).ok, true);
        }

        // commit_group by plugin type → both config_swap_probe instances flip.
        c.send({ type: 'cmd', id: 72, name: 'commit_group',
                 args: { plugin: 'config_swap_probe' } });
        const cg = await c.nextNonLog();
        assert.equal(cg.ok, true, 'commit_group by plugin ok');
        const names = parse(cg.data).results.map(r => r.name).sort();
        assert.deepEqual(names, ['sw_a', 'sw_b'], 'selector expanded to both instances');

        assert.equal((await status(c, 73, 'sw_a')).active, 5, 'sw_a flipped via selector');
        assert.equal((await status(c, 74, 'sw_b')).active, 5, 'sw_b flipped via selector');
    });
});

test('commit_group with an unknown instance reports per-target failure', { timeout: 30000 }, async () => {
    await withBackend(async (c) => {
        await setup(c);
        c.send({ type: 'cmd', id: 50, name: 'commit_group',
                 args: { instances: ['sw_a', 'nope'] } });
        const cg = await c.nextNonLog();
        assert.equal(cg.ok, false, 'overall failure when a target is missing');
        const cgd = parse(cg.data);
        const byName = Object.fromEntries(cgd.results.map(r => [r.name, r.ok]));
        assert.equal(byName['sw_a'], true, 'known target still committed');
        assert.equal(byName['nope'], false, 'unknown target reported failed');
    });
});
