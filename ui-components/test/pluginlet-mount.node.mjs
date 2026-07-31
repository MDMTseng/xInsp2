// pluginlet-mount.node.mjs — the RUNTIME consumer of a plugin's "pluginlets"
// declaration (doc 37). Pure-Node with a tiny fake DOM + an injected registry, so
// it verifies the dispatch/slot/lifecycle logic without the frontend toolchain.
import { test } from "node:test";
import assert from "node:assert/strict";

class El {
  constructor(tag) {
    this.tagName = tag.toUpperCase(); this.children = []; this.dataset = {};
    this.className = ""; this.ownerDocument = doc;
  }
  appendChild(c) { this.children.push(c); c.parentNode = this; return c; }
  remove() { const p = this.parentNode; if (!p) return; p.children = p.children.filter((x) => x !== this); }
  set innerHTML(v) { if (v === "") this.children = []; }
}
const doc = { createElement: (t) => new El(t) };
globalThis.document = doc;

const { mountPluginlets } = await import("../src/pluginlet-mount.mjs");

const client = { async getInstanceDef() { return {}; }, async setInstanceDef() {} };
const newHost = () => { const h = doc.createElement("div"); h.ownerDocument = doc; return h; };

test("mounts each declared pluginlet, in order, into its own slot", async () => {
  const seen = [];
  const registry = {
    controls: async (slot, o) => { seen.push(["controls", o.instance]); return { destroy() { seen.push(["destroy", "controls"]); } }; },
    "live-view": async (slot) => { seen.push(["live-view"]); return { destroy() { seen.push(["destroy", "live-view"]); } }; },
  };
  const host = newHost();
  const r = await mountPluginlets(host, { client, instance: "cd", pluginlets: ["controls", "live-view"], registry });

  assert.deepEqual(seen, [["controls", "cd"], ["live-view"]], "mounted in declaration order");
  assert.equal(host.children.length, 2, "one slot per plet");
  assert.deepEqual(host.children.map((c) => c.dataset.pluginlet), ["controls", "live-view"],
    "each slot is tagged with its plet name");
  assert.deepEqual(r.mounted.map((m) => m.name), ["controls", "live-view"]);

  r.destroy();
  assert.deepEqual(seen.slice(-2), [["destroy", "controls"], ["destroy", "live-view"]],
    "destroy cascades to every mounted panel");
  assert.equal(host.children.length, 0, "host cleared");
});

test("a plet that declines (returns null) leaves no empty slot", async () => {
  // e.g. the controls plet returns null when the instance declares no $schema.
  const registry = { controls: async () => null };
  const host = newHost();
  const r = await mountPluginlets(host, { client, instance: "x", pluginlets: ["controls"], registry });
  assert.equal(r.mounted.length, 0, "nothing counted as mounted");
  assert.equal(host.children.length, 0, "the unused slot is removed");
});

test("an unknown plet name is reported, not fatal — the rest still mount", async () => {
  const missing = [];
  const registry = { controls: async () => ({}) };
  const host = newHost();
  const r = await mountPluginlets(host, {
    client, instance: "cd", pluginlets: ["ghost", "controls"], registry,
    onMissing: (n) => missing.push(n),
  });
  assert.deepEqual(missing, ["ghost"], "the unregistered plet is reported");
  assert.deepEqual(r.mounted.map((m) => m.name), ["controls"], "the known plet still mounted");
});

test("no declaration is a no-op", async () => {
  const host = newHost();
  const r = await mountPluginlets(host, { client, instance: "x", registry: {} });
  assert.equal(r.mounted.length, 0);
  assert.equal(host.children.length, 0);
});
