//
// ws-client.mjs — the shared, GENERIC WS-client shim for the xInsp2 UI component
// library. Framework-agnostic: the xi-* web components AND any external webapp /
// plugin webUI (library-import) use this so nobody reimplements the transport.
//
// It owns ONLY the generic protocol surface: connect + fail-fast version check,
// request/response correlation (`cmd`/`exchange` + convenience verbs), the
// `instances`/`log`/`event` text subscriptions, connection lifecycle events
// (`open`/`close`, the latter tagged when the backend rejected us as busy), and a
// raw binary passthrough (`onBinary`). It does NOT decode application frames — a
// plugin's own webUI decodes the binary frames it asked for. See
// docs/reference/ws-protocol.md.
//

// WS close code the serve.mjs reverse-proxy uses to relay a backend
// `503 single-client-busy` upgrade reject to a browser (which cannot read the
// HTTP status itself). Application range (4000-4999). This is the ONE definition:
// hmi/serve.mjs imports it from here rather than re-declaring a copy to hand-sync.
export const BUSY_CLOSE_CODE = 4003;

// The canonical "does the backend look like a real semver build" probe used by
// `connect({checkVersion: true})`. Exported so callers can reference the one
// source of truth instead of re-pasting the literal (it was copy-pasted at ~7
// call sites). A caller that needs a stricter/looser rule still passes its own
// RegExp/string/function to checkVersion — see connect().
export const XI_VERSION_RE = /\d+\.\d+\.\d+/;

// Classify why a socket closed. `busy` means the backend refused us because
// another client already owns it (single-client enforcement, ws-protocol.md
// "Single-client enforcement") — a retry-when-they-leave condition, distinct from
// "backend down". Detectable two ways: the node `ws` lib surfaces the raw 503 +
// `X-Xi-Reason` header (captured in `upgradeReject`); a browser only sees the
// proxy's BUSY_CLOSE_CODE / reason relayed on the close event.
function classifyClose(closeEvt, upgradeReject) {
  const code = closeEvt && typeof closeEvt.code === "number" ? closeEvt.code : null;
  const reason = (closeEvt && closeEvt.reason) || "";
  if (upgradeReject && upgradeReject.busy) return { busy: true, code, reason: "single-client-busy" };
  if (code === BUSY_CLOSE_CODE || /single-client-busy/i.test(reason)) {
    return { busy: true, code, reason: reason || "single-client-busy" };
  }
  return { busy: false, code, reason };
}

export class XiClient {
  /**
   * @param {string} url  e.g. "ws://127.0.0.1:7823/"
   * @param {{WebSocketImpl?: any}} [opts]  inject a WebSocket impl (node tests)
   */
  constructor(url, opts = {}) {
    this.url = url;
    this._WS = opts.WebSocketImpl || (typeof WebSocket !== "undefined" ? WebSocket : null);
    if (!this._WS) throw new Error("no WebSocket implementation (pass opts.WebSocketImpl in node)");
    this.ws = null;
    this._id = 0;
    this._pending = new Map();          // id -> {resolve, reject}
    this._listeners = {                 // type -> Set<cb>
      instances: new Set(), log: new Set(), event: new Set(),
      binary: new Set(), open: new Set(), close: new Set(),
    };
    // NOTE: no `hello` slot — the backend's hello frame arrives as
    // {type:"event", name:"hello"} and is delivered through the `event` set
    // (dispatch keys off msg.type); a type-level hello listener could never fire.
  }

  // Open the socket; resolves once it's open. If opts.checkVersion is set, also
  // runs `cmd:version` and rejects on mismatch (fail-fast on protocol drift).
  //   checkVersion: true | RegExp | string | (info) => boolean
  //     true       → test info.version against the canonical XI_VERSION_RE
  //     RegExp/str → test/compare info.version
  //     function   → the caller decides from the full version info object
  // Emits an `open` event on connect and a `close` event on disconnect; the close
  // payload is {busy, code, reason} (busy = single-client rejection, see
  // classifyClose). Rejects on any failure BEFORE open (bad URL, refused, 503
  // busy) with an Error carrying `.busy`/`.reason`, so a caller that never got an
  // `open` still gets one settled promise.
  //
  // opts.retry (opt-in) re-attempts a BEFORE-open failure (refused / closed
  // before open):
  //   retry: { attempts, delayMs, busy }
  //     attempts → total tries incl. the first (default 1 = no retry)
  //     delayMs  → wait between tries (default 0)
  //     busy     → also wait out a single-client-busy reject (default true —
  //                the slot is held by another client and frees when they leave);
  //                set false to surface a busy reject immediately.
  // A post-open failure (version mismatch) is NOT retried — retrying can't fix a
  // wrong build. With no `retry` the semantics are exactly the single-attempt
  // default. Auto-reconnect (after a healthy connection later drops) is still the
  // caller's job; retry only covers the initial connect.
  connect(opts = {}) {
    const { retry } = opts;
    if (!retry) return this._connectOnce(opts);
    const attempts = Math.max(1, Number(retry.attempts) || 1);
    const delayMs = Math.max(0, Number(retry.delayMs) || 0);
    const retryBusy = retry.busy !== false;   // busy waits-out by default
    return (async () => {
      let lastErr;
      for (let n = 1; n <= attempts; n++) {
        try { return await this._connectOnce(opts); }
        catch (e) {
          lastErr = e;
          // Only a genuine pre-open connection failure is retriable; a version
          // mismatch (or any post-open error) has no `beforeOpen` flag and falls
          // straight through. A busy reject is retriable unless opted out.
          const retriable = e && e.beforeOpen && (retryBusy || !e.busy);
          if (!retriable || n >= attempts) throw e;
          if (delayMs > 0) await new Promise((r) => setTimeout(r, delayMs));
        }
      }
      throw lastErr;   // unreachable: the loop always returns or throws above
    })();
  }

  // One connection attempt — the original single-shot connect. `connect()` wraps
  // this with the optional retry loop; everything else calls it directly.
  _connectOnce(opts = {}) {
    return new Promise((resolve, reject) => {
      let ws, opened = false;
      try { ws = new this._WS(this.url); } catch (e) { reject(e); return; }
      ws.binaryType = "arraybuffer";
      this.ws = ws;
      // The node `ws` lib exposes the failed HTTP upgrade response (statusCode +
      // headers); the browser WebSocket does not. Where available, capture the
      // 503 single-client-busy reject so `close` can be tagged busy at the source.
      let upgradeReject = null;
      if (typeof ws.on === "function") {
        ws.on("unexpected-response", (_req, res) => {
          const reason = res && res.headers && res.headers["x-xi-reason"];
          upgradeReject = { statusCode: res && res.statusCode, reason,
                            busy: res && res.statusCode === 503 && reason === "single-client-busy" };
        });
      }
      ws.onmessage = (ev) => this._onMessage(ev);
      // Settle a BEFORE-open failure exactly once. `error` and `close` can arrive
      // in either order (or only one — undici fires just `error`, browsers fire
      // `close`), so whichever comes first classifies and settles; both use
      // `upgradeReject` so the verdict is order-independent. Emits one `close` so
      // consumers always get a lifecycle event to reconnect from.
      let failedBeforeOpen = false;
      const failBeforeOpen = (ev) => {
        if (failedBeforeOpen) return;
        failedBeforeOpen = true;
        const info = classifyClose(ev, upgradeReject);
        this._emit("close", info);
        const e = new Error(info.busy ? "single-client-busy: another client owns the backend"
                                      : "connection failed before open");
        e.busy = info.busy; e.reason = info.reason; e.code = info.code;
        e.beforeOpen = true;   // marks this as a retriable pre-open failure (see connect())
        reject(e);
      };
      ws.onerror = () => {
        for (const { reject: rj } of this._pending.values()) rj(new Error("socket error"));
        this._pending.clear();
        if (!opened) failBeforeOpen(null);
      };
      ws.onclose = (ev) => {
        for (const { reject: rj } of this._pending.values()) rj(new Error("socket closed"));
        this._pending.clear();
        if (!opened) { failBeforeOpen(ev); return; }
        this._emit("close", classifyClose(ev, upgradeReject));
      };
      ws.onopen = async () => {
        opened = true;
        this._emit("open", { url: this.url });
        try {
          if (opts.checkVersion) {
            const info = await this.cmd("version");
            const v = info && info.version;
            // `true` selects the canonical probe; otherwise the caller's own
            // function / RegExp / exact-string rule applies.
            const check = opts.checkVersion === true ? XI_VERSION_RE : opts.checkVersion;
            const ok = typeof check === "function"
              ? check(info)
              : (check instanceof RegExp ? check.test(v) : v === check);
            if (!ok) { reject(new Error(`backend version mismatch: got ${v}`)); ws.close(); return; }
          }
          resolve(this);
        } catch (e) { reject(e); }
      };
    });
  }

  _onMessage(ev) {
    const data = ev.data;
    // Raw binary frame — hand the bytes straight through; consumers decode their
    // own application frames (this client stays protocol-generic).
    if (data instanceof ArrayBuffer || (typeof Buffer !== "undefined" && data instanceof Buffer)) {
      this._emit("binary", data);
      return;
    }
    let msg;
    try { msg = JSON.parse(typeof data === "string" ? data : data.toString()); } catch { return; }
    if (msg.type === "rsp") {
      const p = this._pending.get(msg.id);
      if (p) {
        this._pending.delete(msg.id);
        if (msg.ok) p.resolve(this._parseData(msg.data));
        else p.reject(new Error(msg.error || "command failed"));
      }
      return;
    }
    if (this._listeners[msg.type]) this._emit(msg.type, msg);
  }

  _parseData(d) {
    if (typeof d !== "string") return d;
    const t = d.trim();
    if (t.startsWith("{") || t.startsWith("[")) { try { return JSON.parse(t); } catch { /* keep string */ } }
    return d;
  }

  // --- request/response ---------------------------------------------------
  cmd(name, args) {
    const id = ++this._id;
    return new Promise((resolve, reject) => {
      this._pending.set(id, { resolve, reject });
      const m = { type: "cmd", id, name };
      if (args !== undefined) m.args = args;
      try { this.ws.send(JSON.stringify(m)); }
      catch (e) { this._pending.delete(id); reject(e); }
    });
  }

  // --- convenience verbs --------------------------------------------------
  ping()                          { return this.cmd("ping"); }
  version()                       { return this.cmd("version"); }
  listInstances()                 { return this.cmd("list_instances"); }
  getInstanceDef(name)            { return this.cmd("get_instance_def", { name }); }
  setInstanceDef(name, def)       { return this.cmd("set_instance_def", { name, def }); }
  exchange(name, cmd)             { return this.cmd("exchange_instance", { name, cmd }); }
  // Single-parameter set (doc 31): a named script param → raw JSON value; the
  // plugin validates (rsp ok / error). Used by the manifest param-panel.
  setParam(name, value)           { return this.cmd("set_param", { name, value }); }
  getState(name)                  { return this.cmd("get_state", { name }); }
  prepareInstance(name, def, folder) {
    const args = { name, def };
    if (folder !== undefined) args.folder = folder;
    return this.cmd("prepare_instance", args);
  }
  // sel: { instances?, group?, plugin? }
  commitGroup(sel)                { return this.cmd("commit_group", sel); }
  run(args)                       { return this.cmd("run", args); }

  // --- subscriptions (return an unsubscribe fn) ---------------------------
  on(type, cb) {
    const set = this._listeners[type];
    if (!set) throw new Error(`unknown event type: ${type}`);
    set.add(cb);
    return () => set.delete(cb);
  }
  onInstances(cb) { return this.on("instances", cb); }
  onLog(cb)       { return this.on("log", cb); }
  onEvent(cb)     { return this.on("event", cb); }
  // Raw binary passthrough: handler(data) gets the ArrayBuffer/Buffer untouched.
  onBinary(cb)    { return this.on("binary", cb); }
  // Connection lifecycle. onOpen(cb): cb({url}). onClose(cb): cb({busy, code,
  // reason}) — `busy` true means single-client rejection (retry when they leave).
  onOpen(cb)      { return this.on("open", cb); }
  onClose(cb)     { return this.on("close", cb); }

  _emit(type, payload) { for (const cb of this._listeners[type]) { try { cb(payload); } catch { /* listener threw */ } } }

  close() { try { this.ws && this.ws.close(); } catch { /* already closed */ } }
}
