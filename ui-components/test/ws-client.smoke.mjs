// ws-client.smoke.mjs — smoke-test the shared WS shim (task #72) against a real
// backend. node 22 has a global WebSocket, so the browser shim runs as-is here.
//
//   node ui-components/test/ws-client.smoke.mjs
//
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import { XiClient } from "../src/ws-client.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const backendExe = resolve(__dirname, "../../backend/build/Release/xinsp-backend.exe");
const port = 38000 + Math.floor(Math.random() * 20000);

test("XiClient drives the orchestrator verbs end-to-end", { timeout: 30000 }, async () => {
  const child = spawn(backendExe, [`--port=${port}`], { stdio: ["ignore", "ignore", "inherit"] });
  let c;
  try {
    // Wait for the port to accept, then connect with a version check.
    for (let i = 0; i < 40 && !c; i++) {
      await sleep(150);
      try { c = await new XiClient(`ws://127.0.0.1:${port}/`).connect({ checkVersion: /\d+\.\d+\.\d+/ }); }
      catch { c = null; }
    }
    assert.ok(c, "connected + version check passed");

    assert.equal((await c.ping()).pong, true, "ping");

    await c.cmd("create_project", { folder: resolve(tmpdir(), `xi_shim_${Date.now()}`), name: "shim" });
    // create_instance resolves + loads the plugin by name itself (the standalone
    // `load_plugin` WS command was retired at THE CUT, v12 — cmd_create_instance_
    // now calls plugin_mgr.load_plugin internally, surfacing any load error).
    await c.cmd("create_instance", { name: "sw0", plugin: "config_swap_probe" });

    // created → state "created"
    assert.equal((await c.getState("sw0")).state, "created", "fresh instance is created");

    // set def → active; get_instance_def round-trips
    await c.setInstanceDef("sw0", { value: 3 });
    assert.equal((await c.getState("sw0")).state, "active", "active after set_def");
    assert.equal((await c.getInstanceDef("sw0")).value, 3, "get_instance_def reads back 3");

    // prepare (stage) does not move live; commit_group swaps it
    await c.prepareInstance("sw0", { value: 9 });
    assert.equal((await c.getInstanceDef("sw0")).value, 3, "still live on 3 after prepare");
    const cg = await c.commitGroup({ instances: ["sw0"] });
    assert.ok(cg.results.every((r) => r.ok), "commit_group ok");
    assert.equal((await c.getInstanceDef("sw0")).value, 9, "live on 9 after commit_group");

    console.log("shim smoke: all verbs OK");
  } finally {
    if (c) c.close();
    if (child.exitCode === null) { child.kill(); await sleep(100); }
  }
});
