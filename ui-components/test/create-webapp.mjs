// create-webapp.mjs test — the export-B scaffold generator (task #79) seeds an
// external library-import webapp project (vendored bundle, version-pinned WS,
// example composition).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync, rmSync } from "node:fs";
import { resolve, join } from "node:path";
import { tmpdir } from "node:os";
import { createWebapp } from "../export/create-webapp.mjs";

test("createWebapp scaffolds a runnable library-import project", () => {
  const outDir = resolve(tmpdir(), `xi_webapp_${Date.now()}`);
  try {
    const r = createWebapp({ outDir, name: "line1-hmi", url: "ws://plc-host:7823/" });
    for (const f of ["index.html", "app.mjs", "package.json", "README.md", "xi-components.esm.js"])
      assert.ok(existsSync(join(outDir, f)), `${f} written`);

    const app = readFileSync(join(outDir, "app.mjs"), "utf8");
    assert.match(app, /from "\.\/xi-components\.esm\.js"/, "imports the library");
    assert.match(app, /mountPanel/, "composes a control panel");
    assert.match(app, /ws:\/\/plc-host:7823\//, "ws url baked in");
    assert.match(app, /checkVersion/, "version-pinned connect");

    const pkg = JSON.parse(readFileSync(join(outDir, "package.json"), "utf8"));
    assert.equal(pkg.name, "line1-hmi", "package name set");
    assert.equal(pkg.type, "module");
    assert.deepEqual(r.files.length, 5);
  } finally {
    try { rmSync(outDir, { recursive: true }); } catch {}
  }
});
