// build-monitor.mjs test — the export-A bundle generator (task #78) writes a
// self-contained static folder (index.html + the component bundle).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync, rmSync } from "node:fs";
import { resolve, join } from "node:path";
import { tmpdir } from "node:os";
import { buildMonitor } from "../export/build-monitor.mjs";

test("buildMonitor emits index.html + the bundle with the items baked in", () => {
  const outDir = resolve(tmpdir(), `xi_monitor_${Date.now()}`);
  try {
    const r = buildMonitor({
      outDir, url: "ws://host:9000/", title: "Line 1",
      items: [{ type: "value", key: "count", label: "Count" },
              { type: "image", key: "frame", label: "Frame" }],
    });
    assert.ok(existsSync(join(outDir, "index.html")), "index.html written");
    assert.ok(existsSync(join(outDir, "xi-components.esm.js")), "bundle copied");

    const html = readFileSync(join(outDir, "index.html"), "utf8");
    assert.match(html, /ws:\/\/host:9000\//, "url baked in");
    assert.match(html, /"key":"count"/, "items baked in");
    assert.match(html, /mountMonitor/, "renders via mountMonitor");
    assert.match(html, /<title>Line 1<\/title>/, "title set");
    assert.deepEqual(r.files, ["index.html", "xi-components.esm.js"]);
  } finally {
    try { rmSync(outDir, { recursive: true }); } catch {}
  }
});
