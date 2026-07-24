// ws-client.health.mjs — real-backend smoke for the canonical health/state
// contract (schema xi.health/1), the exact wire the HMI (hmi/app.mjs setHealth)
// and the VS Code extension now consume on connect. Kept separate from
// ws-client.smoke.mjs because that test exercises the config_swap_probe plugin
// (needs its DLL built); this one drives get_health with NO plugin so it proves
// the health wire shape over a real backend in any environment.
//
// Accepts BOTH sides of the feature-detection contract the consumers rely on:
//   * a get_health-capable backend → assert the schema/state/components shape;
//   * an older backend → reject with "unknown command", the EXACT signal the HMI
//     and extension degrade on (no version-sniffing). Either way is a pass, so
//     the test documents the degradation path instead of pinning a backend build.
//
//   node --test ui-components/test/ws-client.health.mjs
//
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import { XiClient } from "../src/ws-client.mjs";
import { backendExe, skipLiveBackendTest } from "./backend-exe.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const port = 38000 + Math.floor(Math.random() * 20000);
const KNOWN_STATES = ["boot", "project_loaded", "running", "degraded", "draining", "fault"];

// Resolve to the parsed data on ok, or {unsupported:true} when the backend
// rejects the command as unknown — mirroring the consumers' feature detection.
async function getHealth(c) {
  try { return await c.cmd("get_health"); }
  catch (e) {
    if (/unknown command/i.test(e && e.message || "")) return { unsupported: true };
    throw e;
  }
}

test("get_health honours the health contract (or degrades cleanly)", { skip: skipLiveBackendTest, timeout: 30000 }, async () => {
  const child = spawn(backendExe, [`--port=${port}`], { stdio: ["ignore", "ignore", "inherit"] });
  let c;
  try {
    // `retry` rides out the backend boot window (refused-before-open) so this
    // test no longer hand-rolls a sleep/connect loop.
    c = await new XiClient(`ws://127.0.0.1:${port}/`)
          .connect({ checkVersion: true, retry: { attempts: 40, delayMs: 150 } });
    assert.ok(c, "connected + version check passed");

    const boot = await getHealth(c);
    if (boot.unsupported) {
      // Older backend: the "unknown command" rsp is the consumers' degrade signal.
      // Prove it's stable and stop here (nothing more to assert).
      console.log("health smoke: backend has no get_health — graceful-degrade path validated");
      return;
    }

    // Boot snapshot: no project open → state "boot", no components. The shape the
    // HMI/extension pull on connect (the delivery guarantee).
    assert.equal(boot.schema, "xi.health/1", "schema stamped");
    assert.equal(boot.state, "boot", "no project -> boot");
    assert.ok(Array.isArray(boot.components), "components is an array");
    assert.equal(boot.components.length, 0, "nothing to report at boot");
    assert.equal(typeof boot.since_ms, "number", "top-level since_ms present");

    // Opening a project advances the machine to project_loaded (also drives a
    // health_changed event) — a transition a consumer surfaces.
    await c.cmd("create_project", { folder: resolve(tmpdir(), `xi_health_${Date.now()}`), name: "health" });
    const loaded = await getHealth(c);
    assert.ok(KNOWN_STATES.includes(loaded.state), `known state (got ${loaded.state})`);
    assert.ok(Array.isArray(loaded.components), "components still an array after open");

    console.log(`health smoke: boot->${loaded.state} OK (${loaded.components.length} components)`);
  } finally {
    if (c) c.close();
    if (child.exitCode === null) { child.kill(); await sleep(100); }
  }
});
