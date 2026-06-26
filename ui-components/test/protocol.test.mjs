// Node test for protocol.mjs decoders. Run: node hmi/test/protocol.test.mjs
import assert from "node:assert";
import { parseVars, decodePreviewFrame, bytesToBase64, restoreNonFiniteDeep } from "../src/protocol.mjs";

let pass = 0;
const t = (name, fn) => { fn(); console.log("ok -", name); pass++; };

t("parseVars maps items by name (items[] key)", () => {
  const { run_id, items } = parseVars(JSON.stringify({
    type: "vars", run_id: 7,
    items: [
      { name: "verdict", kind: "string", value: "OK" },
      { name: "fg_pct", kind: "number", value: 0.51 },
      { name: "result", kind: "image", gid: 100, raw: false },
    ],
  }));
  assert.equal(run_id, 7);
  assert.equal(items.verdict.value, "OK");
  assert.equal(items.fg_pct.value, 0.51);
  assert.equal(items.result.gid, 100);
});

t("parseVars accepts legacy 'vars' key + object input", () => {
  const { items } = parseVars({ run_id: 1, vars: [{ name: "x", kind: "number", value: 3 }] });
  assert.equal(items.x.value, 3);
});

t("parseVars restores non-finite number sentinels back to JS numbers", () => {
  const { items } = parseVars({ run_id: 2, items: [
    { name: "n", kind: "number", value: "NaN" },
    { name: "p", kind: "number", value: "Infinity" },
    { name: "m", kind: "number", value: "-Infinity" },
    { name: "ok", kind: "number", value: 1.5 },
    { name: "s", kind: "string", value: "NaN" },   // a STRING var "NaN" must stay a string
  ]});
  assert.ok(Number.isNaN(items.n.value), "NaN sentinel -> NaN");
  assert.equal(items.p.value, Infinity);
  assert.equal(items.m.value, -Infinity);
  assert.equal(items.ok.value, 1.5);
  assert.equal(items.s.value, "NaN");             // not touched (kind !== number)
});

t("parseVars restores non-finite sentinels nested inside record data", () => {
  const { items } = parseVars({ run_id: 3, items: [
    { name: "meas", kind: "record", data: {
      ratio: "NaN",
      bounds: { lo: "-Infinity", hi: "Infinity" },
      samples: [1.0, "NaN", 2.5],
      label: "NaN-ish",            // a real string that isn't an exact sentinel — keep it
      ok: 4,
    }},
  ]});
  const d = items.meas.data;
  assert.ok(Number.isNaN(d.ratio), "top record field NaN");
  assert.equal(d.bounds.lo, -Infinity, "nested object field");
  assert.equal(d.bounds.hi, Infinity);
  assert.ok(Number.isNaN(d.samples[1]), "array element NaN");
  assert.equal(d.samples[0], 1.0);
  assert.equal(d.label, "NaN-ish", "non-sentinel string untouched");
  assert.equal(d.ok, 4);
});

t("restoreNonFiniteDeep leaves a plain value / non-sentinel string alone", () => {
  assert.equal(restoreNonFiniteDeep(5), 5);
  assert.equal(restoreNonFiniteDeep("hello"), "hello");
  assert.equal(restoreNonFiniteDeep(null), null);
});

t("decodePreviewFrame reads BE header + builds jpeg data URL", () => {
  // header: gid=100, codec=0 (jpeg), w=320, h=240, ch=3 ; payload = 3 bytes
  const buf = new Uint8Array(20 + 3);
  const dv = new DataView(buf.buffer);
  dv.setUint32(0, 100, false);
  dv.setUint32(4, 0, false);
  dv.setUint32(8, 320, false);
  dv.setUint32(12, 240, false);
  dv.setUint32(16, 3, false);
  buf.set([0xff, 0xd8, 0xff], 20);
  const f = decodePreviewFrame(buf);
  assert.equal(f.gid, 100);
  assert.equal(f.width, 320);
  assert.equal(f.height, 240);
  assert.equal(f.channels, 3);
  assert.ok(f.dataUrl.startsWith("data:image/jpeg;base64,"));
  assert.equal(f.dataUrl.split(",")[1], bytesToBase64(new Uint8Array([0xff, 0xd8, 0xff])));
});

t("decodePreviewFrame maps codec 2 -> png", () => {
  const buf = new Uint8Array(20);
  new DataView(buf.buffer).setUint32(4, 2, false);
  assert.ok(decodePreviewFrame(buf).dataUrl.startsWith("data:image/png;base64,"));
});

t("decodePreviewFrame rejects a short frame", () => {
  assert.throws(() => decodePreviewFrame(new Uint8Array(8)));
});

console.log(`\n${pass} passed`);
