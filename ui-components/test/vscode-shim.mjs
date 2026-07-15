// vscode-shim.mjs — the acquireVsCodeApi→XiClient shim (FR-3). Loads a
// representative VS Code-webview-style panel html against a MOCKED XiClient and
// proves the {type:'exchange'} ↔ {type:'status'} bridge round-trips: a panel
// action reaches client.exchange with the right instance+cmd, and the reply is
// delivered back to the panel's message handler as a status envelope.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { JSDOM } from "jsdom";

const { createVsCodeApi, installVsCodeShim } = await import("../src/vscode-shim.mjs");

const PANEL_HTML = readFileSync(
  fileURLToPath(new URL("./fixtures/panel.html", import.meta.url)), "utf8");

// A mock XiClient: records each exchange(instance, cmd) and replies per a queue
// (or a default status). exchange returns a Promise, like the real one.
function mockClient(replies = []) {
  return {
    calls: [],
    replies: [...replies],
    async exchange(instance, cmd) {
      // Normalize to a node-realm plain object — the panel builds `cmd` inside
      // the jsdom realm, whose prototype would trip deepStrictEqual otherwise.
      this.calls.push(JSON.parse(JSON.stringify({ instance, cmd })));
      if (this.replies.length) {
        const r = this.replies.shift();
        if (r instanceof Error) throw r;
        return r;
      }
      return { status: "ok" };
    },
  };
}

// Poll until `cond()` (jsdom delivers window.postMessage on its own timer, so a
// reply lands a macrotask or two after the exchange promise settles).
async function until(win, cond, ms = 500) {
  const t0 = Date.now();
  while (!cond() && Date.now() - t0 < ms) await new Promise((r) => win.setTimeout(r, 1));
  return cond();
}

// Load the representative panel with the shim installed BEFORE its inline script
// runs (a real host installs acquireVsCodeApi before the panel loads).
function loadPanel(client, instance) {
  let shim;
  const dom = new JSDOM(PANEL_HTML, {
    runScripts: "dangerously",
    beforeParse(win) { shim = installVsCodeShim({ client, instance, win }); },
  });
  return { dom, win: dom.window, doc: dom.window.document, shim };
}

test("a data-action click round-trips: exchange(instance, cmd) → status reply to the panel", async () => {
  const client = mockClient([{ status: "ready", threshold: 42 }]);
  const { win, doc } = loadPanel(client, "meas0");

  doc.getElementById("reload").click();          // panel posts {type:'exchange', cmd:{command:'get_status'}}
  await until(win, () => doc.getElementById("statusText").textContent === "ready");

  assert.equal(client.calls.length, 1, "one exchange reached the client");
  assert.deepEqual(client.calls[0], { instance: "meas0", cmd: { command: "get_status" } },
    "correct instance + verbatim cmd");
  // The status reply flowed back into the panel's DOM (it read m.status / m.threshold).
  assert.equal(doc.getElementById("statusText").textContent, "ready", "status body applied");
  assert.equal(doc.getElementById("thresh").value, "42", "body field applied to the input");
});

test("a data-param change sends a single-field set_config exchange", async () => {
  const client = mockClient([{ status: "ok" }]);
  const { win, doc } = loadPanel(client, "cam0");

  const input = doc.getElementById("thresh");
  input.value = "128";
  input.dispatchEvent(new win.Event("change"));
  await until(win, () => client.calls.length >= 1);

  assert.deepEqual(client.calls[0],
    { instance: "cam0", cmd: { command: "set_config", threshold: 128 } },
    "data-param change → set_config with the param value, targeting cam0");
});

test("an exchange rejection surfaces as a status envelope carrying error", async () => {
  const client = mockClient([new Error("plugin refused")]);
  const { win, doc } = loadPanel(client, "meas0");

  doc.getElementById("reload").click();
  await until(win, () => doc.getElementById("errText").textContent === "plugin refused");

  assert.equal(doc.getElementById("errText").textContent, "plugin refused",
    "a rejected exchange is delivered as {type:'status', error}");
});

test("the status envelope is spread at top level with type LAST (never shadowed)", async () => {
  // A body that carries its own `type` must not shadow the 'status' discriminator.
  const client = mockClient();
  const seen = [];
  const win = new JSDOM("<!doctype html><body>").window;
  win.addEventListener("message", (e) => seen.push(e.data));
  const api = createVsCodeApi({ client, instance: "x0", win });

  client.replies.push({ type: "measurement", value: 7 });
  api.postMessage({ type: "exchange", cmd: { command: "get" } });
  await until(win, () => seen.length >= 1);

  assert.equal(seen.length, 1, "one reply posted");
  assert.equal(seen[0].type, "status", "envelope type wins over the body's own type field");
  assert.equal(seen[0].value, 7, "body fields are spread at the top level");
});

test("non-exchange messages are ignored (only the exchange verb bridges)", async () => {
  const client = mockClient();
  const api = createVsCodeApi({ client, instance: "x0", win: new JSDOM("<!doctype html>").window });
  api.postMessage({ type: "ready" });
  api.postMessage({ foo: "bar" });
  api.postMessage(null);
  await new Promise((r) => setTimeout(r, 0));
  assert.equal(client.calls.length, 0, "nothing reached the client");
});

test("getState / setState persist through the window's sessionStorage", async () => {
  const client = mockClient();
  const win = new JSDOM("<!doctype html>", { url: "http://localhost" }).window;
  const api = createVsCodeApi({ client, instance: "x0", win });
  assert.equal(api.getState(), null, "no state initially");
  api.setState({ tab: 3 });
  assert.deepEqual(api.getState(), { tab: 3 }, "state round-trips");
});

test("installVsCodeShim exposes acquireVsCodeApi and uninstall restores", async () => {
  const client = mockClient();
  const win = new JSDOM("<!doctype html>").window;
  assert.equal(typeof win.acquireVsCodeApi, "undefined");
  const { uninstall } = installVsCodeShim({ client, instance: "x0", win });
  assert.equal(typeof win.acquireVsCodeApi, "function", "acquireVsCodeApi installed");
  assert.equal(typeof win.acquireVsCodeApi().postMessage, "function", "returns the api");
  uninstall();
  assert.equal(typeof win.acquireVsCodeApi, "undefined", "uninstall restores");
});
