//
// serve.mjs — tunnel-friendly HMI runner: spawns the backend headless on the demo
// project, serves this hmi/ folder, AND reverse-proxies the WebSocket at /ws to
// the backend. Everything is one origin / one port, so a single HTTP tunnel
// (e.g. WebTunnelHub) exposes both the page and the live WS — the page connects
// to a same-origin wss://…/ws with no extra config.
//
//   node hmi/serve.mjs                 (needs `ws`: run with NODE_PATH pointing at
//                                        vscode-extension/node_modules, see hub note)
//
import http from "node:http";
import { spawn } from "node:child_process";
import { readFile, mkdir } from "node:fs/promises";
import { fileURLToPath, pathToFileURL } from "node:url";
import { dirname, join, normalize, extname, sep } from "node:path";
import { createRequire } from "node:module";
// Single source of truth for the busy close code (shim <-> proxy contract).
import { BUSY_CLOSE_CODE } from "../ui-components/src/ws-client.mjs";

const HMI = dirname(fileURLToPath(import.meta.url));
const REPO = dirname(HMI);
// `ws` isn't installed under hmi/; borrow it from the VS Code extension's deps.
// (ESM bare-import resolution ignores NODE_PATH, so resolve via createRequire.)
const require = createRequire(pathToFileURL(join(REPO, "vscode-extension", "package.json")));
const { WebSocketServer, WebSocket } = require("ws");
const BACKEND = join(REPO, "backend", "build", "Release", "xinsp-backend.exe");
// Project to run headless. Defaults to hmi/demo; override with PROJECT=<path>
// (absolute, or relative to the repo root) to drive a different dashboard.
const DEMO = process.env.PROJECT
  ? (process.env.PROJECT.match(/^([a-zA-Z]:|\/|\\)/) ? process.env.PROJECT : join(REPO, process.env.PROJECT))
  : join(HMI, "demo");
const PORT = +(process.env.PORT || 8770);
const WS_PORT = +(process.env.WS_PORT || 7872);
const FPS = +(process.env.FPS || 5);
const BE_URL = `ws://127.0.0.1:${WS_PORT}/`;

const MIME = { ".html": "text/html", ".mjs": "text/javascript", ".js": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml", ".png": "image/png" };

// ---- spawn the backend headless (continuous via --autostart-fps) ------------
const iso = join(process.env.LOCALAPPDATA || process.env.TEMP, "Temp", "xi_hmi_tunnel_tmp");
await mkdir(iso, { recursive: true });   // Windows temp_directory_path() throws if TEMP is absent
const be = spawn(BACKEND, [`--port=${WS_PORT}`, `--project=${DEMO}`, `--autostart-fps=${FPS}`],
  { cwd: REPO, env: { ...process.env, TEMP: iso, TMP: iso }, stdio: "ignore" });
be.on("exit", (c) => console.log(`[serve] backend exited (${c})`));

// ---- static file server (confined to hmi/) ---------------------------------
const server = http.createServer(async (req, res) => {
  try {
    let p = decodeURIComponent((req.url || "/").split("?")[0]);
    if (p === "/" || p === "") p = "/index.html";
    const full = normalize(join(HMI, p));
    if (!full.startsWith(HMI + sep)) { res.writeHead(403).end("forbidden"); return; }
    const body = await readFile(full);
    res.writeHead(200, { "content-type": MIME[extname(full)] || "application/octet-stream", "cache-control": "no-store" });
    res.end(body);
  } catch { res.writeHead(404).end("not found"); }
});

// ---- WebSocket reverse-proxy: browser <-> /ws <-> backend -------------------
const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false });
server.on("upgrade", (req, sock, head) => {
  console.log(`[ws] upgrade url=${req.url} upgrade=${req.headers.upgrade} origin=${req.headers.origin || "-"} ext=${req.headers["sec-websocket-extensions"] || "-"}`);
  if (!(req.url || "").startsWith("/ws")) { console.log("[ws] not /ws -> destroy"); sock.destroy(); return; }
  wss.handleUpgrade(req, sock, head, (client) => {
    console.log("[ws] browser handshake OK -> dialing backend");
    const up = new WebSocket(BE_URL);          // one backend conn per browser
    const queue = [];
    up.on("open", () => { console.log("[ws] backend open"); for (const [d, b] of queue) up.send(d, { binary: b }); queue.length = 0; });
    // The backend rejects a 2nd connection with `503 single-client-busy`. A browser
    // can't read that HTTP status, so relay it as a distinct WS close code the shim
    // recognises — otherwise the operator sees a generic "disconnected" retry loop
    // when the real cause is "another client owns the backend" (review 10 #6).
    up.on("unexpected-response", (_req, res) => {
      const busy = res.statusCode === 503 && res.headers["x-xi-reason"] === "single-client-busy";
      console.log(`[ws] backend upgrade rejected status=${res.statusCode} reason=${res.headers["x-xi-reason"] || "-"}`);
      try { client.close(busy ? BUSY_CLOSE_CODE : 1011, busy ? "single-client-busy" : "backend upgrade failed"); } catch {}
      try { up.close(); } catch {}
    });
    client.on("message", (d, isBin) => { up.readyState === 1 ? up.send(d, { binary: isBin }) : queue.push([d, isBin]); });
    up.on("message", (d, isBin) => { if (client.readyState === 1) client.send(d, { binary: isBin }); });
    const bye = (who) => (e) => { console.log(`[ws] close via ${who}${e && e.message ? " " + e.message : ""}`); try { client.close(); } catch {} try { up.close(); } catch {} };
    client.on("close", bye("browser")); client.on("error", bye("browser-err"));
    up.on("close", bye("backend")); up.on("error", bye("backend-err"));
  });
});

server.listen(PORT, "127.0.0.1", () => {
  console.log(`[serve] HMI on http://127.0.0.1:${PORT}/  (page + /ws proxy -> ${BE_URL}, project=${DEMO} @${FPS}fps)`);
  console.log(`[serve] tunnel this one port; the page uses a same-origin /ws.`);
});
for (const sig of ["SIGINT", "SIGTERM"]) process.on(sig, () => { try { be.kill(); } catch {} process.exit(0); });
