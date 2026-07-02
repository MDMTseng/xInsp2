// ws-client.busy.mjs — the shared shim's connection-lifecycle + single-client
// (503 "busy") handling (review 10 #6). Two detection paths:
//   1. node `ws` lib: the failed HTTP upgrade is visible (statusCode + headers)
//      via an `unexpected-response` event.
//   2. browser: the HTTP status is NOT visible, so the serve.mjs proxy relays the
//      backend 503 as a distinct WS close code (BUSY_CLOSE_CODE); the shim reads
//      that off the close event instead.
// The unit tests drive both with fake sockets (no network); the last test drives a
// real backend to prove single-client enforcement surfaces as a connect reject.
//
//   node --test ui-components/test/ws-client.busy.mjs
//
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { XiClient, BUSY_CLOSE_CODE } from "../src/ws-client.mjs";

// --- fake sockets -----------------------------------------------------------
// Browser-style: only on* handler props, no EventEmitter `.on` (so the shim's
// `unexpected-response` capture is skipped, exactly like a real browser).
class BrowserWS {
  constructor(url) { this.url = url; this.binaryType = ""; BrowserWS.last = this; }
  close() {}
  _open() { this.onopen && this.onopen(); }
  _close(code, reason) { this.onclose && this.onclose({ code, reason }); }
}
// node `ws`-lib-style: adds `.on(evt, cb)` including `unexpected-response`.
class NodeWS {
  constructor(url) { this.url = url; this.binaryType = ""; this._on = {}; NodeWS.last = this; }
  on(evt, cb) { this._on[evt] = cb; }
  close() {}
  _unexpected(statusCode, reason) {
    this._on["unexpected-response"] && this._on["unexpected-response"]({}, { statusCode, headers: { "x-xi-reason": reason } });
  }
  _close(code, reason) { this.onclose && this.onclose({ code, reason }); }
}

test("node ws-lib path: a 503 single-client-busy upgrade is classified busy", async () => {
  const c = new XiClient("ws://x/", { WebSocketImpl: NodeWS });
  let closeInfo = null;
  c.onClose((i) => { closeInfo = i; });
  const p = c.connect();
  NodeWS.last._unexpected(503, "single-client-busy");  // the ws lib sees the HTTP reply
  NodeWS.last._close(1006, "");                         // then the socket closes
  await assert.rejects(p, (e) => e.busy === true && /single-client-busy/.test(e.message));
  assert.equal(closeInfo.busy, true, "close event tagged busy");
});

test("browser/proxy path: a BUSY_CLOSE_CODE close is classified busy", async () => {
  const c = new XiClient("ws://x/", { WebSocketImpl: BrowserWS });
  let closeInfo = null;
  c.onClose((i) => { closeInfo = i; });
  const p = c.connect();
  BrowserWS.last._close(BUSY_CLOSE_CODE, "single-client-busy");
  await assert.rejects(p, (e) => e.busy === true);
  assert.equal(closeInfo.busy, true);
  assert.equal(closeInfo.code, BUSY_CLOSE_CODE);
});

test("a plain failure before open is NOT busy (backend down, not owned)", async () => {
  const c = new XiClient("ws://x/", { WebSocketImpl: BrowserWS });
  let closeInfo = null;
  c.onClose((i) => { closeInfo = i; });
  const p = c.connect();
  BrowserWS.last._close(1006, "");
  await assert.rejects(p, (e) => e.busy === false);
  assert.equal(closeInfo.busy, false);
});

test("open then close emits open + a non-busy close (normal disconnect)", async () => {
  const c = new XiClient("ws://x/", { WebSocketImpl: BrowserWS });
  let opened = false, closeInfo = null;
  c.onOpen(() => { opened = true; });
  c.onClose((i) => { closeInfo = i; });
  const p = c.connect();
  BrowserWS.last._open();
  await p;                       // resolves once open (no checkVersion)
  assert.ok(opened, "open event fired");
  BrowserWS.last._close(1000, "bye");
  assert.equal(closeInfo.busy, false);
  assert.equal(closeInfo.code, 1000);
});

// --- integration: a real backend rejects the 2nd client ---------------------
// Native WebSocket (undici) can't read the 503 status — that's the platform
// limit the proxy exists to paper over — so here we only assert the shim surfaces
// the single-client rejection as a settled (rejected) connect, not a hang.
const backendExe = resolve(dirname(fileURLToPath(import.meta.url)), "../../backend/build/Release/xinsp-backend.exe");
const port = 38000 + Math.floor(Math.random() * 20000);

test("real backend: the 2nd client's connect rejects (single-client enforced)", { timeout: 30000 }, async () => {
  const child = spawn(backendExe, [`--port=${port}`], { stdio: ["ignore", "ignore", "inherit"] });
  let a;
  try {
    for (let i = 0; i < 40 && !a; i++) {
      await sleep(150);
      try { a = await new XiClient(`ws://127.0.0.1:${port}/`).connect(); } catch { a = null; }
    }
    assert.ok(a, "first client connected");
    assert.equal((await a.ping()).pong, true, "first client is live");

    // Second client must not hang and must not succeed while A holds the slot.
    const b = new XiClient(`ws://127.0.0.1:${port}/`);
    const settled = await Promise.race([
      b.connect().then(() => "opened", (e) => `rejected:${e.message}`),
      sleep(5000).then(() => "timeout"),
    ]);
    assert.ok(settled.startsWith("rejected:"), `2nd connect rejected, got "${settled}"`);

    assert.equal((await a.ping()).pong, true, "first client still live after 2nd was refused");
  } finally {
    if (a) a.close();
    if (child.exitCode === null) { child.kill(); await sleep(100); }
  }
});
