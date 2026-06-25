// _serve.mjs — e2e helpers: a tiny static file server + a backend spawner, so a
// Playwright spec can serve a folder and point a page at a live backend.
import http from "node:http";
import { spawn } from "node:child_process";
import { readFile } from "node:fs/promises";
import { setTimeout as sleep } from "node:timers/promises";
import { join, normalize, extname, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";

export const REPO = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
export const BACKEND = join(REPO, "backend", "build", "Release", "xinsp-backend.exe");

const MIME = {
  ".html": "text/html", ".mjs": "text/javascript", ".js": "text/javascript",
  ".json": "application/json", ".css": "text/css", ".svg": "image/svg+xml",
  ".png": "image/png", ".wasm": "application/wasm",
};

// Serve `root` on an ephemeral port. Returns { url, close }.
export async function serveStatic(root) {
  const server = http.createServer(async (req, res) => {
    try {
      const rel = normalize(decodeURIComponent(new URL(req.url, "http://x").pathname)).replace(/^[/\\]+/, "");
      const path = join(root, rel || "index.html");
      const body = await readFile(path);
      res.writeHead(200, { "content-type": MIME[extname(path)] || "application/octet-stream" });
      res.end(body);
    } catch { res.writeHead(404); res.end("not found"); }
  });
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  const port = server.address().port;
  return { url: `http://127.0.0.1:${port}`, close: () => new Promise((r) => server.close(r)) };
}

// Spawn the backend on an ephemeral port. opts: { project, fps } — when a project
// is given the backend opens it (cwd = REPO so plugin dirs resolve) and, with
// fps, autostarts streaming. Otherwise a temp cwd so relative folders don't
// pollute the repo. Returns { port, proc, stop }.
export async function startBackend(opts = {}) {
  const port = 41000 + Math.floor(Math.random() * 20000);
  const args = [`--port=${port}`];
  let cwd;
  if (opts.project) {
    args.push(`--project=${opts.project}`);
    if (opts.fps) args.push(`--autostart-fps=${opts.fps}`);
    cwd = REPO;
  } else {
    cwd = join(tmpdir(), `xi_e2e_${port}`);
    await (await import("node:fs/promises")).mkdir(cwd, { recursive: true });
  }
  const proc = spawn(BACKEND, args, { cwd, stdio: ["ignore", "ignore", "inherit"] });
  // wait until the WS server accepts a connection
  for (let i = 0; i < 60; i++) {
    await sleep(150);
    if (proc.exitCode !== null) break;
    const ok = await new Promise((r) => {
      const ws = new WebSocket(`ws://127.0.0.1:${port}/`);
      ws.onopen = () => { try { ws.close(); } catch {} r(true); };
      ws.onerror = () => r(false);
    });
    if (ok) break;
  }
  return { port, proc, stop: () => { try { proc.kill(); } catch {} } };
}
