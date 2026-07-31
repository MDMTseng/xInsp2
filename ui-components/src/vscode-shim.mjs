//
// vscode-shim.mjs — the OFFICIAL `acquireVsCodeApi` → `XiClient` compatibility
// adapter (integration_test FR-3). A plugin's VS Code-webview-style
// `ui/index.html` — one that calls `acquireVsCodeApi()` and talks the
// `{type:'exchange',cmd}` ↔ `{type:'status',…}` bridge with `data-param` /
// `data-action` conventions (doc 31 "Plugin ↔ its own UI"; plugin-ui.md context
// A) — runs UNCHANGED in a plain browser over WS, by installing this shim as the
// window's `acquireVsCodeApi` before the panel loads.
//
// The bridge is exactly the one the real VS Code host implements and the app
// team's `bridge_stub.html` validated:
//   panel → api.postMessage({type:'exchange', cmd})       (the ONLY verb bridged)
//   shim  → XiClient.exchange(instance, cmd)               (id-correlated WS cmd)
//   shim  → window.postMessage({type:'status', ...reply})  (delivered as e.data)
//   panel ← window 'message' → reads e.data (checks type:'status', reads fields)
//
// The reply is a `type:'status'` ENVELOPE with the plugin's status body SPREAD
// at the top level: panels that gate on `e.data.type === 'status'` (aravis,
// ct_shape_based_matching) AND panels that read body fields directly
// (dim_measure) both work off the one shape. `type` is written LAST so a status
// body carrying its own `type` field can never shadow the envelope discriminator.
//
// getState/setState are sessionStorage-backed, matching the webview's persistence
// contract (a panel that stashes UI state across reloads keeps working).
//
// The envelope shape itself (statusEnvelope / isExchange) lives in the shared
// exchange-envelope.mjs so the real VS Code host builds the byte-identical shape.
//

import { statusEnvelope, isExchange } from "./exchange-envelope.mjs";

const STATE_KEY = "__xi_vscode_state__";

/**
 * Build one `vscode`-API object bound to a single instance + XiClient.
 * @param {{client, instance:string, win?:Window, onSend?:(cmd:any)=>void}} opts
 *   client  — an XiClient (or anything with `exchange(instance, cmd) → Promise`)
 *   instance— the instance id this panel controls (its exchange target)
 *   win     — the window to post replies onto / read sessionStorage from
 *             (default globalThis; pass an iframe's contentWindow for a vendored
 *             panel mounted in an iframe)
 *   onSend  — optional hook, called with each outgoing `cmd` (debug / tests)
 * @returns the `vscode` object: { postMessage, getState, setState }.
 */
export function createVsCodeApi(opts = {}) {
  const { client, instance, onSend } = opts;
  const win = opts.win || globalThis;
  if (!client || typeof client.exchange !== "function")
    throw new Error("createVsCodeApi: opts.client needs an exchange(instance, cmd) method");
  if (!instance) throw new Error("createVsCodeApi: opts.instance (the panel's instance id) is required");

  return {
    // The panel's ONLY outbound channel. Non-exchange messages are ignored (the
    // real host bridge only forwards exchange; other verbs are host details).
    postMessage(msg) {
      if (!isExchange(msg)) return;
      if (onSend) { try { onSend(msg.cmd); } catch { /* hook threw */ } }
      // Defer so the reply arrives as an async 'message' event (matches a real
      // webview and avoids re-entrancy inside the panel's send handler).
      Promise.resolve()
        .then(() => client.exchange(instance, msg.cmd))
        .then((reply) => win.postMessage(statusEnvelope(reply), "*"))
        .catch((err) =>
          win.postMessage({ type: "status", error: String((err && err.message) || err) }, "*"));
    },
    getState() {
      try {
        const s = win.sessionStorage && win.sessionStorage.getItem(STATE_KEY);
        return s ? JSON.parse(s) : null;
      } catch { return null; }
    },
    setState(state) {
      try { win.sessionStorage && win.sessionStorage.setItem(STATE_KEY, JSON.stringify(state)); }
      catch { /* storage disabled */ }
      return state;
    },
  };
}

/**
 * Install the shim as `win.acquireVsCodeApi`, so a vendored plugin ui/index.html
 * that does `const vscode = acquireVsCodeApi()` runs unchanged. Call BEFORE the
 * panel's script executes (e.g. an iframe `beforeParse` hook, or on the page
 * before injecting the panel).
 * @param {{client, instance:string, win?:Window, onSend?}} opts
 * @returns {{api, uninstall:()=>void}}
 */
export function installVsCodeShim(opts = {}) {
  const win = opts.win || globalThis;
  const api = createVsCodeApi(opts);
  const prev = win.acquireVsCodeApi;
  // VS Code's real acquireVsCodeApi throws on a second call; we return the same
  // bound api idempotently — friendlier, and panels only call it once anyway.
  win.acquireVsCodeApi = () => api;
  return {
    api,
    uninstall() {
      if (prev) win.acquireVsCodeApi = prev;
      else { try { delete win.acquireVsCodeApi; } catch { win.acquireVsCodeApi = undefined; } }
    },
  };
}
