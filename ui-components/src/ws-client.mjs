//
// ws-client.mjs — the shared WS-client shim for the xInsp2 UI component library
// (task #72). Framework-agnostic: the xi-* web components AND any external webapp
// (library-import) use this so nobody reimplements the protocol.
//
// Covers: connect + fail-fast version check, request/response correlation, the
// orchestrator/config verbs, `vars`/`instances`/`log`/`event` subscriptions, and
// decoded binary preview frames. Browser-first (uses the global WebSocket); a
// node test can inject one via opts.WebSocketImpl.
//
// See docs/roadmap/webui-and-ui-export.md + docs/reference/ws-protocol.md.
//

import { parseVars, decodePreviewFrame } from "./protocol.mjs";

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
      vars: new Set(), instances: new Set(), log: new Set(),
      event: new Set(), preview: new Set(), hello: new Set(),
    };
    this._imgSubs = new Map();           // var name -> open-viewer count
  }

  // --- preview subscription (ref-counted) ---------------------------------
  // The backend sends NO image previews by default — encode + transmit only
  // happens for names someone is actually viewing. A component calls
  // subscribeImage(name) when it shows an image and unsubscribeImage(name) when
  // it hides/unmounts; we ref-count so M viewers of one name → one subscription,
  // and push the union to the backend. Re-asserted on (re)connect.
  subscribeImage(name) {
    if (!name) return;
    const n = (this._imgSubs.get(name) || 0) + 1;
    this._imgSubs.set(name, n);
    if (n === 1) this._pushImageSubs();
  }
  unsubscribeImage(name) {
    if (!name) return;
    const n = (this._imgSubs.get(name) || 0) - 1;
    if (n <= 0) { if (this._imgSubs.delete(name)) this._pushImageSubs(); }
    else this._imgSubs.set(name, n);
  }
  _pushImageSubs() {
    if (!this.ws || this.ws.readyState !== 1) return;   // re-asserted on connect
    this.cmd("subscribe", { names: [...this._imgSubs.keys()] }).catch(() => {});
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
          // Re-assert any standing image subscriptions (the backend defaults to
          // send-none and clears subs for a fresh connection).
          if (this._imgSubs.size) this._pushImageSubs();
          resolve(this);
        } catch (e) { reject(e); }
      };
    });
  }

  _onMessage(ev) {
    const data = ev.data;
    // Binary preview frame.
    if (data instanceof ArrayBuffer || (typeof Buffer !== "undefined" && data instanceof Buffer)) {
      try { this._deliverPreview(decodePreviewFrame(data)); } catch { /* short frame */ }
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
    if (msg.type === "vars") { this._emit("vars", parseVars(msg)); return; }
    if (this._listeners[msg.type]) this._emit(msg.type, msg);
  }

  // Decode each preview frame ONCE here and share the GC-managed <img> to every
  // consumer (the backend already sends one frame per canon image, so this is one
  // JPEG decode per image instead of one per component). Consumers hold the
  // reference while they show it; GC reclaims when nothing references it — no
  // close, no ring. In non-browser/jsdom (tests) we skip the decode and emit the
  // dataUrl as before; a consumer there falls back to decoding it itself.
  _deliverPreview(frame) {
    const realBrowser = typeof window !== "undefined" && typeof Image !== "undefined"
      && !/jsdom/i.test((typeof navigator !== "undefined" && navigator.userAgent) || "");
    if (!realBrowser) { this._emit("preview", frame); return; }
    const img = new Image();
    let done = false;
    const fire = () => { if (done) return; done = true; this._emit("preview", frame); };
    img.onload  = () => { frame.image = img; fire(); };   // shared decoded handle
    img.onerror = fire;                                    // fall back to dataUrl
    img.src = frame.dataUrl;
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
  onVars(cb)      { return this.on("vars", cb); }
  onInstances(cb) { return this.on("instances", cb); }
  onLog(cb)       { return this.on("log", cb); }
  onEvent(cb)     { return this.on("event", cb); }
  onPreview(cb)   { return this.on("preview", cb); }

  _emit(type, payload) { for (const cb of this._listeners[type]) { try { cb(payload); } catch { /* listener threw */ } } }

  close() { try { this.ws && this.ws.close(); } catch { /* already closed */ } }
}
