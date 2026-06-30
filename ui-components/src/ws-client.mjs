//
// ws-client.mjs — the shared, GENERIC WS-client shim for the xInsp2 UI component
// library. Framework-agnostic: the xi-* web components AND any external webapp /
// plugin webUI (library-import) use this so nobody reimplements the transport.
//
// It owns ONLY the generic protocol surface: connect + fail-fast version check,
// request/response correlation (`cmd`/`exchange` + convenience verbs), the
// `instances`/`log`/`event` text subscriptions, and a raw binary passthrough
// (`onBinary`). It does NOT decode application frames — a plugin's own webUI
// decodes the binary frames it asked for. See docs/reference/ws-protocol.md.
//

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
      instances: new Set(), log: new Set(), event: new Set(), hello: new Set(),
      binary: new Set(),
    };
  }

  // Open the socket; resolves once it's open. If opts.checkVersion is set, also
  // runs `cmd:version` and rejects on mismatch (fail-fast on protocol drift).
  //   checkVersion: (info) => boolean | RegExp | string   (string/RegExp tests info.version)
  connect(opts = {}) {
    return new Promise((resolve, reject) => {
      let ws;
      try { ws = new this._WS(this.url); } catch (e) { reject(e); return; }
      ws.binaryType = "arraybuffer";
      this.ws = ws;
      ws.onmessage = (ev) => this._onMessage(ev);
      ws.onerror = (ev) => {
        for (const { reject: rj } of this._pending.values()) rj(new Error("socket error"));
        this._pending.clear();
      };
      ws.onclose = () => {
        for (const { reject: rj } of this._pending.values()) rj(new Error("socket closed"));
        this._pending.clear();
      };
      ws.onopen = async () => {
        try {
          if (opts.checkVersion) {
            const info = await this.cmd("version");
            const v = info && info.version;
            const ok = typeof opts.checkVersion === "function"
              ? opts.checkVersion(info)
              : (opts.checkVersion instanceof RegExp ? opts.checkVersion.test(v) : v === opts.checkVersion);
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

  _emit(type, payload) { for (const cb of this._listeners[type]) { try { cb(payload); } catch { /* listener threw */ } } }

  close() { try { this.ws && this.ws.close(); } catch { /* already closed */ } }
}
