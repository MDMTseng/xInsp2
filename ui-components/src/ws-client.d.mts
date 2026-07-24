// Type declarations for ws-client.mjs (the shared, generic WS client — plain JS
// consumed by the xi-* web components, plugin webUIs, AND the VS Code extension).
// Keeps `tsc --noEmit` clean on the extension side without compiling the .mjs.
// Only the surface the TS consumers touch is declared; see ws-client.mjs for the
// full contract.

// WS close code the reverse-proxy uses to relay a backend 503 single-client-busy.
export const BUSY_CLOSE_CODE: number;
// Canonical "does the backend look like a real semver build" probe.
export const XI_VERSION_RE: RegExp;

export interface XiClientOpts {
  /** Inject a WebSocket implementation (required in node; e.g. the `ws` pkg). */
  WebSocketImpl?: unknown;
}

export interface XiRetryOpts {
  attempts?: number;
  delayMs?: number;
  busy?: boolean;
}

export interface XiConnectOpts {
  checkVersion?: boolean | RegExp | string | ((info: unknown) => boolean);
  retry?: XiRetryOpts;
}

/** Why a socket closed. `busy` = single-client rejection (retry when they leave). */
export interface XiCloseInfo {
  busy: boolean;
  code: number | null;
  reason: string;
}

export class XiClient {
  constructor(url: string, opts?: XiClientOpts);

  /** The live underlying socket (the injected WebSocketImpl instance), or null. */
  ws: { readyState: number; close(): void } | null;
  url: string;

  /** Open the socket; resolves once open (and version-checked, if requested). */
  connect(opts?: XiConnectOpts): Promise<XiClient>;

  /** Request/response: send a cmd, resolve with its rsp data (reject on error). */
  cmd(name: string, args?: unknown): Promise<any>;

  /** Subscribe to a lifecycle/message channel; returns an unsubscribe fn. */
  on(type: string, cb: (payload: any) => void): () => void;
  onOpen(cb: (info: { url: string }) => void): () => void;
  onClose(cb: (info: XiCloseInfo) => void): () => void;
  onEvent(cb: (msg: any) => void): () => void;
  onInstances(cb: (msg: any) => void): () => void;
  onLog(cb: (msg: any) => void): () => void;
  onBinary(cb: (data: ArrayBuffer | Buffer) => void): () => void;

  close(): void;
}
