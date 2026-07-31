import WebSocket from 'ws';
import { EventEmitter } from 'events';
import { XiClient } from '../../ui-components/src/ws-client.mjs';

export interface WsClientOptions {
    url: string;
    reconnectMs?: number;
    /** Optional shared secret — sent as `Authorization: Bearer <secret>` in
     *  the handshake. Required when the backend was started with --auth. */
    authSecret?: string;
}

// XiClient (the shared generic client the web components + plugin webUIs use)
// owns the wire protocol; this class is now a thin node-side adapter around it.
// It keeps its long-standing public surface — the `open`/`close`/`json`/`binary`
// events, id-supplied `sendCmd`, `setUrl`/`getUrl`, steady-state auto-reconnect —
// so extension.ts (its only consumer) is untouched, while the actual transport,
// request/response correlation, JSON dispatch and 503-busy handling all run
// through the same XiClient code as every other frontend.
//
// Two things live HERE rather than in XiClient because they are node/extension
// specifics XiClient deliberately leaves to the caller:
//   1. Auth — a browser WebSocket can't set request headers, so we inject a
//      subclass of the `ws` package that adds `Authorization: Bearer <secret>`.
//   2. Steady-state reconnect — XiClient.retry only covers the initial connect;
//      reconnect-on-drop after a healthy session is the wrapper's job (below).
export class WsClient extends EventEmitter {
    private xi: XiClient | null = null;
    private url: string;
    private reconnectMs: number;
    private authSecret?: string;
    private closed = false;
    private readonly WsImpl: unknown;

    constructor(opts: WsClientOptions) {
        super();
        this.url = opts.url;
        this.reconnectMs = opts.reconnectMs ?? 2000;
        this.authSecret = opts.authSecret;
        this.WsImpl = makeWebSocketImpl(this.authSecret);
    }

    // Update the target URL before connect() — used when the extension resolves a
    // free backend port at spawn time (managed mode auto-port).
    setUrl(url: string): void { this.url = url; }
    getUrl(): string { return this.url; }

    connect(): void {
        if (this.closed) return;
        const xi = new XiClient(this.url, { WebSocketImpl: this.WsImpl });
        this.xi = xi;

        xi.onOpen(() => this.emit('open'));
        xi.onClose(() => {
            // XiClient emits exactly one `close` per connection (pre-open failure
            // OR post-open drop). Mirror the original's reconnect-on-close loop —
            // any close while not disposed schedules a fresh connect after the
            // backoff, so a bounced/respawned backend is re-attached automatically.
            this.emit('close');
            if (!this.closed) {
                setTimeout(() => this.connect(), this.reconnectMs);
            }
        });
        // Re-emit every inbound application frame as the flat `json` event the
        // extension's dispatcher expects. XiClient consumes `rsp` frames into its
        // own correlation map (they resurface via sendCmd below), so only the
        // unsolicited channels — events, instance lists, log lines — pass through
        // here; that is exactly the set the extension's `json` handler routes.
        xi.onEvent((msg) => this.emit('json', msg));
        xi.onInstances((msg) => this.emit('json', msg));
        xi.onLog((msg) => this.emit('json', msg));
        xi.onBinary((data) => this.emit('binary', Buffer.isBuffer(data) ? data : Buffer.from(data)));

        // The wrapper drives reconnect off the `close` event, so a pre-open
        // connect failure is expected here — swallow the rejection.
        xi.connect().catch(() => { /* close handler schedules the retry */ });
    }

    // Fire a command with a CALLER-supplied id and surface its reply as a
    // synthetic `{type:'rsp', id, ...}` json event, so the extension's existing
    // id-keyed pending map keeps working unchanged. XiClient owns the on-the-wire
    // id + correlation; the caller's `id` is a purely local dispatch token.
    sendCmd(id: number, name: string, args?: Record<string, unknown>): void {
        if (!this.connected) return;   // mirror the original no-op-when-down
        this.xi!.cmd(name, args)
            .then((data) => this.emit('json', { type: 'rsp', id, ok: true, data }))
            .catch((err) => this.emit('json', {
                type: 'rsp', id, ok: false,
                error: err && err.message ? err.message : String(err),
            }));
    }

    dispose(): void {
        this.closed = true;
        if (this.xi) {
            try { this.xi.close(); } catch {}
            this.xi = null;
        }
    }

    get connected(): boolean {
        return !!this.xi && !!this.xi.ws && this.xi.ws.readyState === WebSocket.OPEN;
    }
}

// Build the WebSocket implementation XiClient should construct. Without auth this
// is just the `ws` package class. With auth we return a subclass that pins the
// Authorization header on every handshake — XiClient calls `new Impl(url)` with a
// single arg (no header hook), so the secret has to ride in via the constructor.
function makeWebSocketImpl(authSecret?: string): unknown {
    if (!authSecret) return WebSocket;
    return class AuthWebSocket extends WebSocket {
        constructor(url: string) {
            super(url, { headers: { Authorization: `Bearer ${authSecret}` } });
        }
    };
}
