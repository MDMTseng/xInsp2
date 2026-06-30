// xInsp2 WebSocket protocol — TypeScript side.
//
// Mirrors backend/include/xi/xi_protocol.hpp. The canonical schema lives
// in protocol/messages.md. Shared fixtures in protocol/fixtures/*.json are
// parsed by both sides to prove the contract holds.
//
// This is the GENERIC transport layer only: cmd / rsp / event / log / instances.
// Any plugin-specific payload decoding (image previews, exposed values, …) lives
// in the plugin's own webUI, not here.

export interface Cmd {
    type: 'cmd';
    id: number;
    name: string;
    args?: Record<string, unknown>;
}

export interface RspOk {
    type: 'rsp';
    id: number;
    ok: true;
    data?: unknown;
}

export interface RspErr {
    type: 'rsp';
    id: number;
    ok: false;
    error?: string;
}

export type Rsp = RspOk | RspErr;

export interface InstancesMsg {
    type: 'instances';
    instances: Array<{ name: string; plugin: string; def: Record<string, unknown> }>;
    params: Array<{
        name: string;
        type: 'int' | 'float' | 'bool';
        value: number | boolean;
        min?: number;
        max?: number;
    }>;
}

export interface LogMsg {
    type: 'log';
    level: 'debug' | 'info' | 'warn' | 'error';
    msg: string;
    ts?: number;
}

export interface EventMsg {
    type: 'event';
    name: string;
    data?: Record<string, unknown>;
}

export type ServerMessage = Rsp | InstancesMsg | LogMsg | EventMsg;
export type ClientMessage = Cmd;

// ---------- helpers ----------

export function parseServerMessage(text: string): ServerMessage {
    const obj = JSON.parse(text);
    if (!obj || typeof obj !== 'object' || typeof obj.type !== 'string') {
        throw new Error('not a protocol message');
    }
    return obj as ServerMessage;
}

export function encodeClientMessage(msg: ClientMessage): string {
    return JSON.stringify(msg);
}
