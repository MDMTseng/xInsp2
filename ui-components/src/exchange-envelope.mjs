//
// exchange-envelope.mjs — the ONE shared shape of the plugin ↔ its-own-UI
// webview bridge, so the browser shim (vscode-shim.mjs) and the real VS Code
// host (vscode-extension) build/parse the SAME envelope from the SAME code.
//
// The bridge has exactly two verbs, and this module owns both directions:
//   panel → { type:'exchange', cmd }   the ONLY inbound verb  → isExchange()
//   host  → { type:'status', ...body } the reply envelope     → statusEnvelope()
//
// The reply is a `type:'status'` ENVELOPE with the plugin's status body SPREAD
// at the top level: panels that gate on `e.data.type === 'status'` (aravis,
// ct_shape_based_matching) AND panels that read body fields directly
// (dim_measure) both work off the one shape. `type` is written LAST so a status
// body carrying its own `type` field can never shadow the envelope discriminator.
//

// Wrap an exchange reply as the `{type:'status', …body}` message a panel expects.
export function statusEnvelope(reply) {
  if (reply && typeof reply === "object" && !Array.isArray(reply)) {
    return { ...reply, type: "status" };   // type LAST: never shadowed by a body field
  }
  // A non-object reply (rare — exchange bodies are status JSON): keep it reachable.
  return { type: "status", value: reply };
}

// The inbound guard: is this the one verb the bridge forwards? A panel's only
// outbound message is `{type:'exchange', cmd}`; every other message is ignored.
export function isExchange(msg) {
  return !!(msg && msg.type === "exchange" && msg.cmd);
}
