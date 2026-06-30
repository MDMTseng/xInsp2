//
// create-webapp.mjs — scaffold an external LIBRARY-IMPORT webapp (export mode B,
// task #79). B is bespoke: rather than auto-generating a full app, we seed an
// external project that owns its repo/stack and imports the components, with WS
// pre-wired (version-pinned) and an example composition (control panel + viewer)
// the integrator then develops. The component bundle is vendored so the scaffold
// runs offline immediately; the README shows the npm-package route too. See
// docs/roadmap/webui-and-ui-export.md (consumption models, shape B).
//
//   import { createWebapp } from "./export/create-webapp.mjs";
//   createWebapp({ outDir, name, url });
//   // CLI: node export/create-webapp.mjs <outDir> [name] [wsUrl]
//
import { mkdirSync, copyFileSync, writeFileSync, existsSync } from "node:fs";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const BUNDLE = resolve(__dirname, "../dist/xi-components.esm.js");

const INDEX_HTML = (name) => `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>${name}</title>
  <style>
    :root { --xi-accent: #6366f1; }
    body { font: 14px system-ui, sans-serif; margin: 1.5rem; }
    xi-image-viewer { width: 100%; height: 360px; border: 1px solid #e5e7eb; border-radius: 6px; }
    section.app { display: grid; gap: 1rem; max-width: 64rem; }
  </style>
  <script type="module" src="./app.mjs"></script>
</head>
<body>
  <h1>${name}</h1>
  <div id="status">connecting…</div>
  <section class="app">
    <div id="panel"></div>
    <xi-image-viewer id="viewer"></xi-image-viewer>
  </section>
</body>
</html>
`;

const APP_MJS = (url) => `// app.mjs — this app owns the composition. The components are library-imported;
// swap the relative bundle for "@xinsp2/components" once you add a build step.
import { XiClient, mountPanel } from "./xi-components.esm.js";

// Descriptor-as-data: the UI schema is just JSON this app owns (could be fetched
// from the plugin's manifest instead). Edit freely.
const PANEL = [{
  section: "Tuning", tag: "control", controls: [
    { type: "slider", key: "threshold", label: "Threshold", min: 0, max: 255 },
  ],
}];

const status = (t) => (document.getElementById("status").textContent = t);

(async () => {
  try {
    // Version binding: pin the protocol; fail fast on a mismatched backend.
    const client = await new XiClient("${url}").connect({ checkVersion: /\\d+\\.\\d+\\.\\d+/ });
    status("connected ${url}");

    // The <xi-image-viewer> is a generic component — feed it from whatever frame
    // source your app/plugin decodes (e.g. client.onBinary(buf => ...)). The WS
    // client is transport-generic and does NOT decode application frames for you.
    const viewer = document.getElementById("viewer");

    // An editable control panel bound to a real instance.
    await mountPanel(document.getElementById("panel"), { client, instance: "INSTANCE_NAME", descriptor: PANEL })
      .catch((e) => console.warn("panel:", e.message));
  } catch (e) { status("connect failed: " + e.message); }
})();
`;

const PKG = (name) => JSON.stringify({
  name, version: "0.1.0", private: true, type: "module",
  scripts: { serve: "npx --yes serve ." },
  // For a build-pipeline project, replace the vendored bundle with the package:
  //   "dependencies": { "@xinsp2/components": "^0.1.0" }
}, null, 2) + "\n";

const README = (name, url) => `# ${name}

An external xInsp2 webapp (export mode B — library-import). It owns its own
repo/stack and imports the xi-components library; the backend is driven over WS.

## Run

\`\`\`
npm run serve        # or serve this folder any other way; it's static
\`\`\`

Open the page; it connects to \`${url}\` (edit in app.mjs).

## How it's wired

- \`xi-components.esm.js\` is vendored so this runs offline with no install. For a
  build pipeline, install \`@xinsp2/components\` and import from it instead.
- \`app.mjs\` owns the composition: an \`mountPanel\` control panel (descriptor =
  plain JSON you edit) and a generic \`xi-image-viewer\` you feed from whatever
  frame source your app/plugin decodes.
- The WS connection pins the protocol version (\`checkVersion\`) and fails fast on a
  mismatched backend.

Set \`INSTANCE_NAME\` in app.mjs to a real instance, and the panel keys to its def.
`;

export function createWebapp({ outDir, name = "xinsp2-webapp", url = "ws://127.0.0.1:7823/" }) {
  if (!existsSync(BUNDLE)) throw new Error(`build the library first (npm run build) — missing ${BUNDLE}`);
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "index.html"), INDEX_HTML(name));
  writeFileSync(join(outDir, "app.mjs"), APP_MJS(url));
  writeFileSync(join(outDir, "package.json"), PKG(name));
  writeFileSync(join(outDir, "README.md"), README(name, url));
  copyFileSync(BUNDLE, join(outDir, "xi-components.esm.js"));
  return { outDir, files: ["index.html", "app.mjs", "package.json", "README.md", "xi-components.esm.js"] };
}

// CLI
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  const [outDir, name, url] = process.argv.slice(2);
  if (!outDir) { console.error("usage: node export/create-webapp.mjs <outDir> [name] [wsUrl]"); process.exit(2); }
  const r = createWebapp({ outDir, name, url });
  console.log("scaffolded", r.files.length, "files →", r.outDir);
}
