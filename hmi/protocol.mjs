//
// protocol.mjs — pure decoders for the xInsp2 WS protocol (no DOM, no WS).
// Shared by the browser app and the node test. See docs/protocol.md.
//

// Parse a `vars` text message into { run_id, items } where items is a
// name -> item map (item = { name, kind, value?, gid?, raw? }).
export function parseVars(msg) {
  const o = typeof msg === "string" ? JSON.parse(msg) : msg;
  const list = o.items || o.vars || [];
  const items = {};
  for (const it of list) items[it.name] = it;
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
