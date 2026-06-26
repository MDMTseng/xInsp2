// xInsp2 WebSocket protocol — TypeScript side.
//
// Mirrors backend/include/xi/xi_protocol.hpp. The canonical schema lives
// in protocol/messages.md. Shared fixtures in protocol/fixtures/*.json are
// parsed by both sides to prove the contract holds.

export type VarKind =
    | 'image'
    | 'number'
    | 'boolean'
    | 'string'
    | 'json'
    | 'custom'
    | 'record';

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

export interface VarItemImage {
    name: string;
    kind: 'image';
    gid: number;
    /** Canonical gid of this image's dedup group — vars sharing one buffer
     *  report a common `src` and the backend sends a single preview frame for it
     *  (under the canonical var, where `src === gid`). Mirror the frame onto the
     *  group via `src`. For a non-duplicated image `src === gid`. Optional:
     *  absent on pre-dedup history snapshots — treat a missing `src` as `gid`. */
    src?: number;
    raw: boolean;
}

export interface VarItemNumber {
    name: string;
    kind: 'number';
    value: number;
}

export interface VarItemBoolean {
    name: string;
    kind: 'boolean';
    value: boolean;
}

export interface VarItemString {
    name: string;
    kind: 'string';
    value: string;
}

export interface VarItemJson {
    name: string;
    kind: 'json' | 'custom';
    value: unknown;
}

export interface VarItemRecord {
    name: string;
    kind: 'record';
    /** The record's scalar fields as a JSON object. */
    data: unknown;
    /** Keys of sub-images carried by the record. */
    image_keys?: string[];
    /** Map of sub-image key -> gid (gated by the record var's name, like images). */
    images?: Record<string, number>;
}

export type VarItem =
    | VarItemImage
    | VarItemNumber
    | VarItemBoolean
    | VarItemString
    | VarItemJson
    | VarItemRecord;

export interface Vars {
    type: 'vars';
    run_id: number;
    items: VarItem[];
}

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

export type ServerMessage = Rsp | Vars | InstancesMsg | LogMsg | EventMsg;
export type ClientMessage = Cmd;

// ---------- binary preview frame ----------

export const PREVIEW_HEADER_SIZE = 20;

export enum Codec {
    JPEG = 0,
    BMP = 1,
    PNG = 2,
}

export interface PreviewHeader {
    gid: number;
    codec: Codec;
    width: number;
    height: number;
    channels: number;
}

export function encodePreviewHeader(h: PreviewHeader): Uint8Array {
    const buf = new Uint8Array(PREVIEW_HEADER_SIZE);
    const dv = new DataView(buf.buffer);
    dv.setUint32(0, h.gid, false);
    dv.setUint32(4, h.codec, false);
    dv.setUint32(8, h.width, false);
    dv.setUint32(12, h.height, false);
    dv.setUint32(16, h.channels, false);
    return buf;
}

export function decodePreviewHeader(buf: ArrayBuffer | Uint8Array): PreviewHeader {
    const bytes = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
    if (bytes.byteLength < PREVIEW_HEADER_SIZE) {
        throw new Error(`preview frame too short: ${bytes.byteLength}`);
    }
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    return {
        gid: dv.getUint32(0, false),
        codec: dv.getUint32(4, false) as Codec,
        width: dv.getUint32(8, false),
        height: dv.getUint32(12, false),
        channels: dv.getUint32(16, false),
    };
}

// ---------- helpers ----------

// Restore the quoted non-finite sentinels the backend emits for doubles
// (NaN/Infinity/-Infinity can't be bare JSON tokens, so they arrive as strings).
// VarItemNumber.value is typed `number` but the wire carries "NaN" etc.; without
// this the value stays a string and any numeric use silently misreads. Mirrors
// restoreNonFinite in ui-components/src/protocol.mjs + the C++ nonfinite_from_str.
export function restoreNonFinite(v: unknown): unknown {
    if (v === 'NaN') { return NaN; }
    if (v === 'Infinity') { return Infinity; }
    if (v === '-Infinity') { return -Infinity; }
    return v;
}

// Recursively restore sentinels anywhere inside a record's `data` (object/array).
export function restoreNonFiniteDeep(v: unknown): unknown {
    if (typeof v === 'string') { return restoreNonFinite(v); }
    if (Array.isArray(v)) { return v.map(restoreNonFiniteDeep); }
    if (v && typeof v === 'object') {
        const o = v as Record<string, unknown>;
        for (const k of Object.keys(o)) { o[k] = restoreNonFiniteDeep(o[k]); }
        return o;
    }
    return v;
}

export function parseServerMessage(text: string): ServerMessage {
    const obj = JSON.parse(text);
    if (!obj || typeof obj !== 'object' || typeof obj.type !== 'string') {
        throw new Error('not a protocol message');
    }
    if (obj.type === 'vars' && Array.isArray(obj.items)) {
        for (const it of obj.items) {
            if (it && it.kind === 'number' && typeof it.value === 'string') {
                it.value = restoreNonFinite(it.value);
            } else if (it && it.kind === 'record' && it.data && typeof it.data === 'object') {
                it.data = restoreNonFiniteDeep(it.data);
            }
        }
    }
    return obj as ServerMessage;
}

export function encodeClientMessage(msg: ClientMessage): string {
    return JSON.stringify(msg);
}
