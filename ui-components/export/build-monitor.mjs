//
// build-monitor.mjs — emit a self-contained STATUS MONITOR bundle (export mode A,
// task #78). Writes <out>/index.html (imports the component bundle, connects via
// XiClient, renders mountMonitor over the configured items) and copies the built
// xi-components.esm.js next to it. Offline-ready: a static folder you can serve
// or open directly. See docs/roadmap/webui-and-ui-export.md.
//
//   import { buildMonitor } from "./export/build-monitor.mjs";
//   buildMonitor({ outDir, url, title, items });
//   // or CLI: node export/build-monitor.mjs <config.json> <outDir>
//
import { mkdirSync, copyFileSync, writeFileSync, existsSync, readFileSync } from "node:fs";
import { resolve, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const BUNDLE = resolve(__dirname, "../dist/xi-components.esm.js");

function htmlFor({ url = "ws://127.0.0.1:7823/", title = "xInsp2 Monitor", items = [], columns = 3 }) {
  const cfg = JSON.stringify({ url, items, columns });
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>${escapeHtml(title)}</title>
  <style>
    :root { --xi-accent: #6366f1; }
    body { font: 14px system-ui, sans-serif; margin: 1.25rem; }
    h1 { font-size: 1.05rem; }
    .xi-tile { border: 1px solid #e5e7eb; border-radius: 8px; padding: .5rem .6rem; }
    .xi-tile-label { font-size: 12px; color: #6b7280; margin-bottom: .25rem; }
    .xi-value { font-size: 1.4rem; font-variant-numeric: tabular-nums; }
    #status { color: #6b7280; font-size: 12px; }
  </style>
  <script type="module">
    import { XiClient, mountMonitor } from "./xi-components.esm.js";
    const CFG = ${cfg};
    const status = (t) => (document.getElementById("status").textContent = t);
    (async () => {
      try {
        const client = await new XiClient(CFG.url).connect({ checkVersion: /\\d+\\.\\d+\\.\\d+/ });
        status("connected " + CFG.url);
        await client.cmd("subscribe", { all: true }).catch(() => {});
        mountMonitor(document.getElementById("grid"), { client, items: CFG.items, columns: CFG.columns });
      } catch (e) { status("connect failed: " + e.message); }
    })();
  </script>
</head>
<body>
  <h1>${escapeHtml(title)}</h1>
  <div id="status">connecting…</div>
  <div id="grid" style="margin-top: 1rem;"></div>
</body>
</html>
`;
}

const escapeHtml = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

export function buildMonitor({ outDir, url, title, items, columns }) {
  if (!existsSync(BUNDLE)) throw new Error(`build the library first (npm run build) — missing ${BUNDLE}`);
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "index.html"), htmlFor({ url, title, items, columns }));
  copyFileSync(BUNDLE, join(outDir, "xi-components.esm.js"));
  return { outDir, files: ["index.html", "xi-components.esm.js"] };
}

// CLI
if (import.meta.url === `file://${process.argv[1]}` || process.argv[1] === fileURLToPath(import.meta.url)) {
  const [cfgPath, outDir] = process.argv.slice(2);
  if (!cfgPath || !outDir) { console.error("usage: node export/build-monitor.mjs <config.json> <outDir>"); process.exit(2); }
  const cfg = JSON.parse(readFileSync(cfgPath, "utf8"));
  const r = buildMonitor({ outDir, ...cfg });
  console.log("wrote", r.files.join(", "), "→", r.outDir);
}
