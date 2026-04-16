import WebSocket from 'ws';
import { EventEmitter } from 'events';

export interface WsClientOptions {
    url: string;
    reconnectMs?: number;
}

export class WsClient extends EventEmitter {
    private ws: WebSocket | null = null;
    private url: string;
    private reconnectMs: number;
    private closed = false;

    constructor(opts: WsClientOptions) {
        super();
        this.url = opts.url;
        this.reconnectMs = opts.reconnectMs ?? 2000;
    }

    connect(): void {
        if (this.closed) return;
        const ws = new WebSocket(this.url);

        ws.on('open', () => {
            this.ws = ws;
            this.emit('open');
        });

        ws.on('message', (data: Buffer | ArrayBuffer | Buffer[], isBinary: boolean) => {
            if (isBinary) {
                const buf = data instanceof Buffer ? data : Buffer.from(data as ArrayBuffer);
                this.emit('binary', buf);
            } else {
                const text = data.toString();
                try {
                    const obj = JSON.parse(text);
                    this.emit('json', obj);
                } catch {
                    this.emit('raw', text);
                }
            }
        });

        ws.on('close', () => {
            this.ws = null;
            this.emit('close');
            if (!this.closed) {
                setTimeout(() => this.connect(), this.reconnectMs);
            }
        });

        ws.on('error', () => {
            // error is followed by close; retry happens there
        });
    }

    sendCmd(id: number, name: string, args?: Record<string, unknown>): void {
        if (!this.ws) return;
        const msg = { type: 'cmd', id, name, args: args ?? {} };
        this.ws.send(JSON.stringify(msg));
    }

    dispose(): void {
        this.closed = true;
        if (this.ws) {
            try { this.ws.close(); } catch {}
            this.ws = null;
        }
    }

    get connected(): boolean {
        return this.ws !== null && this.ws.readyState === WebSocket.OPEN;
    }
}
