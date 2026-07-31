// ws-client.connect-opts.mjs — unit tests for the two connect() options the
// shim consolidated: the canonical `checkVersion: true` probe (XI_VERSION_RE)
// and the opt-in `retry` loop. Pure node, fake WebSocket injection (no network),
// same style as ws-client.busy.mjs.
//
//   node --test ui-components/test/ws-client.connect-opts.mjs
//
import { test } from "node:test";
import assert from "node:assert/strict";
import { XiClient, XI_VERSION_RE, BUSY_CLOSE_CODE } from "../src/ws-client.mjs";

// --- fakes ------------------------------------------------------------------
// A socket that opens and answers `cmd:version` with a scripted version string,
// so the checkVersion branch runs end-to-end. `_open()` is driven by the test.
class VersionWS {
  constructor(url) { this.url = url; this.binaryType = ""; VersionWS.last = this; }
  close() {}
  send(s) {
    const m = JSON.parse(s);
    if (m.name === "version") {
      // rsp.data is the wire string the shim's _parseData JSON-parses back.
      const frame = { type: "rsp", id: m.id, ok: true, data: JSON.stringify({ version: VersionWS.reply }) };
      queueMicrotask(() => this.onmessage && this.onmessage({ data: JSON.stringify(frame) }));
    }
  }
  _open() { this.onopen && this.onopen(); }
}

// A sequenced socket for the retry loop: each successive `new` reads the next
// verdict from `SeqWS.plan` and auto-drives itself on the next tick, so the
// retry loop (which creates one socket per attempt) advances on its own.
class SeqWS {
  constructor(url) {
    this.url = url; this.binaryType = "";
    const verdict = SeqWS.plan[Math.min(SeqWS.n, SeqWS.plan.length - 1)];
    SeqWS.n++;
    setTimeout(() => {
      if (verdict === "open") { this.onopen && this.onopen(); }
      else if (verdict === "busy") { this.onclose && this.onclose({ code: BUSY_CLOSE_CODE, reason: "single-client-busy" }); }
      else { this.onerror && this.onerror(); this.onclose && this.onclose({ code: 1006, reason: "" }); }  // "refuse"
    }, 0);
  }
  static reset(plan) { SeqWS.plan = plan; SeqWS.n = 0; }
  close() {} send() {}
}

// Auto-opens then answers version with a NON-semver string, counting instances —
// proves a version mismatch is a post-open failure that retry must NOT re-attempt.
class BadVersionWS {
  constructor(url) { this.url = url; this.binaryType = ""; BadVersionWS.n++; setTimeout(() => this.onopen && this.onopen(), 0); }
  close() {}
  send(s) {
    const m = JSON.parse(s);
    if (m.name === "version") {
      const frame = { type: "rsp", id: m.id, ok: true, data: JSON.stringify({ version: "not-a-version" }) };
      queueMicrotask(() => this.onmessage && this.onmessage({ data: JSON.stringify(frame) }));
    }
  }
}

// --- checkVersion: true (XI_VERSION_RE) -------------------------------------
test("XI_VERSION_RE is exported and matches a semver, rejects non-semver", () => {
  assert.ok(XI_VERSION_RE.test("1.2.3"), "accepts 1.2.3");
  assert.ok(XI_VERSION_RE.test("xi 0.42.7 (build)"), "matches an embedded semver");
  assert.ok(!XI_VERSION_RE.test("beta"), "rejects a non-numeric version");
});

test("checkVersion:true accepts a semver backend via the canonical probe", async () => {
  VersionWS.reply = "0.12.4";
  const c = new XiClient("ws://x/", { WebSocketImpl: VersionWS });
  const p = c.connect({ checkVersion: true });
  VersionWS.last._open();
  assert.equal(await p, c, "resolves to the client");
});

test("checkVersion:true rejects a non-semver backend", async () => {
  VersionWS.reply = "nightly";
  const c = new XiClient("ws://x/", { WebSocketImpl: VersionWS });
  const p = c.connect({ checkVersion: true });
  VersionWS.last._open();
  await assert.rejects(p, /version mismatch: got nightly/);
});

test("checkVersion still honours a caller RegExp / string / function", async () => {
  // exact-string form
  VersionWS.reply = "9.9.9";
  let c = new XiClient("ws://x/", { WebSocketImpl: VersionWS });
  let p = c.connect({ checkVersion: "9.9.9" });
  VersionWS.last._open();
  await p;

  // function form: gets the whole info object
  VersionWS.reply = "1.0.0";
  c = new XiClient("ws://x/", { WebSocketImpl: VersionWS });
  p = c.connect({ checkVersion: (info) => info.version.startsWith("1.") });
  VersionWS.last._open();
  await p;

  // RegExp form that deliberately fails
  VersionWS.reply = "1.0.0";
  c = new XiClient("ws://x/", { WebSocketImpl: VersionWS });
  p = c.connect({ checkVersion: /^2\./ });
  VersionWS.last._open();
  await assert.rejects(p, /version mismatch/);
});

// --- retry ------------------------------------------------------------------
test("no retry option = a single attempt (unchanged default)", async () => {
  SeqWS.reset(["refuse"]);
  const c = new XiClient("ws://x/", { WebSocketImpl: SeqWS });
  await assert.rejects(c.connect(), /before open/);
  assert.equal(SeqWS.n, 1, "exactly one attempt with no retry");
});

test("retry re-attempts a refused connect until it opens", async () => {
  SeqWS.reset(["refuse", "refuse", "open"]);
  const c = new XiClient("ws://x/", { WebSocketImpl: SeqWS });
  assert.equal(await c.connect({ retry: { attempts: 5, delayMs: 1 } }), c);
  assert.equal(SeqWS.n, 3, "took three attempts");
});

test("retry gives up after `attempts` and rejects with the last error", async () => {
  SeqWS.reset(["refuse"]);
  const c = new XiClient("ws://x/", { WebSocketImpl: SeqWS });
  await assert.rejects(c.connect({ retry: { attempts: 3, delayMs: 1 } }), /before open/);
  assert.equal(SeqWS.n, 3, "exactly `attempts` tries, then stop");
});

test("retry waits out a single-client-busy by default", async () => {
  SeqWS.reset(["busy", "busy", "open"]);
  const c = new XiClient("ws://x/", { WebSocketImpl: SeqWS });
  await c.connect({ retry: { attempts: 5, delayMs: 1 } });
  assert.equal(SeqWS.n, 3, "retried through the busy rejects");
});

test("retry.busy:false surfaces a busy reject immediately (no wait)", async () => {
  SeqWS.reset(["busy", "open"]);
  const c = new XiClient("ws://x/", { WebSocketImpl: SeqWS });
  await assert.rejects(c.connect({ retry: { attempts: 5, delayMs: 1, busy: false } }), (e) => e.busy === true);
  assert.equal(SeqWS.n, 1, "did not retry the busy reject");
});

test("a version mismatch is a post-open failure and is NOT retried", async () => {
  BadVersionWS.n = 0;
  const c = new XiClient("ws://x/", { WebSocketImpl: BadVersionWS });
  await assert.rejects(
    c.connect({ checkVersion: true, retry: { attempts: 5, delayMs: 1 } }),
    /version mismatch/,
  );
  assert.equal(BadVersionWS.n, 1, "opened once; no retry on a wrong build");
});
