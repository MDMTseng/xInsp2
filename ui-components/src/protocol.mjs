//
// protocol.mjs — pure decoders for the xInsp2 WS protocol (no DOM, no WS).
//
// The canonical home for these (hmi/protocol.mjs predates the shared library and
// can re-export from here once it migrates). Pure functions only, so they're
// trivially unit-testable in node and reusable in the browser. See
// docs/reference/ws-protocol.md.
//

// Restore the quoted non-finite sentinels the backend emits for number vars
// (NaN/Infinity can't be bare JSON tokens). Without this a `kind:"number"` var
// arrives with a STRING value "NaN" and any chart/threshold comparison silently
// misreads it. Mirrors the C++ nonfinite_from_str (xi_record.hpp).
export function restoreNonFinite(v) {
  if (v === "NaN") return NaN;
  if (v === "Infinity") return Infinity;
  if (v === "-Infinity") return -Infinity;
  return v;
}

// Recursively restore the non-finite sentinels anywhere inside a record's `data`
// object/array. A plugin can write a NaN/Inf field into a nested Record (a
// divide-by-zero measurement), and the backend emits it as the quoted string
// "NaN" deep inside kind:"record" data — restoring only the top-level number var
// left those as strings, so a threshold/plot on a record field silently misread
// (or threw in Python). Walks plain objects/arrays only; leaves other types.
export function restoreNonFiniteDeep(v) {
  if (typeof v === "string") return restoreNonFinite(v);
  if (Array.isArray(v)) {
    for (let i = 0; i < v.length; i++) v[i] = restoreNonFiniteDeep(v[i]);
    return v;
  }
  if (v && typeof v === "object") {
    for (const k of Object.keys(v)) v[k] = restoreNonFiniteDeep(v[k]);
    return v;
  }
  return v;
}

// Parse a `vars` text message into { run_id, items } where items is a
// name -> item map (item = { name, kind, value?, gid?, raw? }).
export function parseVars(msg) {
  const o = typeof msg === "string" ? JSON.parse(msg) : msg;
  const list = o.items || o.vars || [];
  const items = {};
  for (const it of list) {
    if (it && it.kind === "number" && typeof it.value === "string")
      it.value = restoreNonFinite(it.value);
    else if (it && it.kind === "record" && it.data && typeof it.data === "object")
      it.data = restoreNonFiniteDeep(it.data);
    items[it.name] = it;
  }
  return { run_id: o.run_id, items };
}

const CODEC_MIME = { 0: "image/jpeg", 1: "image/bmp", 2: "image/png" };

// Decode a binary preview frame (Uint8Array / ArrayBuffer) per the 20-byte
// big-endian header: gid, codec, width, height, channels, then payload.
// Returns { gid, codec, width, height, channels, dataUrl }.
export function decodePreviewFrame(buf) {
  const u8 = buf instanceof Uint8Array ? buf : new Uint8Array(buf);
  if (u8.byteLength < 20) throw new Error("preview frame shorter than 20-byte header");
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const gid = dv.getUint32(0, false);
  const codec = dv.getUint32(4, false);
  const width = dv.getUint32(8, false);
  const height = dv.getUint32(12, false);
  const channels = dv.getUint32(16, false);
  const payload = u8.subarray(20);
  const mime = CODEC_MIME[codec] || "application/octet-stream";
  return {
    gid, codec, width, height, channels,
    dataUrl: `data:${mime};base64,${bytesToBase64(payload)}`,
  };
}

// Portable base64 of a Uint8Array — uses Buffer in node, btoa in the browser.
export function bytesToBase64(u8) {
  if (typeof Buffer !== "undefined") return Buffer.from(u8).toString("base64");
  let s = "";
  const CHUNK = 0x8000;
  for (let i = 0; i < u8.length; i += CHUNK)
    s += String.fromCharCode.apply(null, u8.subarray(i, i + CHUNK));
  return btoa(s);
}
