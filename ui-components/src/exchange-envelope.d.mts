// Type declarations for exchange-envelope.mjs (a plain-JS module shared between
// the browser shim and the VS Code extension). Keeps `tsc --noEmit` clean on the
// extension side without compiling the .mjs.

// Wrap an exchange reply as the `{type:'status', …body}` message a panel expects.
// `type` is written LAST so a body field named `type` can never shadow it.
export function statusEnvelope(reply: unknown): { type: 'status'; [k: string]: unknown };

// The inbound guard: is this the one verb the bridge forwards (`{type:'exchange', cmd}`)?
export function isExchange(msg: unknown): boolean;
