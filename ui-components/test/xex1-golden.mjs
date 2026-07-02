// xex1-golden.mjs — cross-implementation test of the expose plugin's XEX1 decoder
// against the C++ encoder's golden frames.
//
// The XEX1 binary frame is encoded in C++ (plugins/expose/src/xex1_encode.hpp) and
// decoded, independently and hand-rolled, by the expose plugin's webUI
// (plugins/expose/ui/index.html) and by examples/lib/xex1.py. This test loads the
// EXACT decoder shipped in index.html — the region between the `// <XEX1-DECODER>`
// markers — and decodes every golden .bin under protocol/fixtures/binary/, asserting
// the decoded content matches the committed manifest. Regenerate the goldens with:
//   cmake --build plugins/build --config Release --target xex1_fixtures_test
//   ./plugins/build/Release/xex1_fixtures_test --regen   (with XINSP2_FIXTURES set)
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..", "..");
const FIX = path.join(REPO, "protocol", "fixtures", "binary");
const HTML = path.join(REPO, "plugins", "expose", "ui", "index.html");

// Extract the shipped decoder verbatim and evaluate it, so we test the exact code
// the webUI runs (not a copy that could drift).
function loadShippedDecoder() {
  const html = readFileSync(HTML, "utf8");
  const m = html.match(/\/\/ <XEX1-DECODER>([\s\S]*?)\/\/ <\/XEX1-DECODER>/);
  assert.ok(m, "XEX1-DECODER marker region not found in index.html");
  // eslint-disable-next-line no-new-func
  const factory = new Function(m[1] + "\nreturn { MsgpackReader, decodeXEX1, restoreNonFinite };");
  return factory();
}

const { decodeXEX1, restoreNonFinite } = loadShippedDecoder();
const manifest = JSON.parse(readFileSync(path.join(FIX, "manifest.json"), "utf8"));

const b64 = (s) => new Uint8Array(Buffer.from(s, "base64"));

for (const f of manifest.frames) {
  if (f.v === 2) {
    test(`XEX1 golden (v2): ${f.file}`, () => {
      const body = decodeXEX1(new Uint8Array(readFileSync(path.join(FIX, f.file))));
      assert.ok(body, `decodeXEX1 returned null for ${f.file}`);
      assert.equal(body.v, 2);
      assert.equal(body.channel, f.channel);
      assert.equal(body.seq, f.seq);
      // The canonical frame dump: scalar/str/mp entries -> values, image
      // descriptors -> images[] with raw pixels (v2 is lossless, no JPEG).
      for (const fld of f.fields) {
        if (fld.kind === "image") {
          const im = body.images.find((x) => x.key === fld.key);
          assert.ok(im, `v2 image entry ${fld.key} missing`);
          assert.equal(im.w, fld.w);
          assert.equal(im.h, fld.h);
          assert.equal(im.c, fld.c);
          assert.deepEqual(new Uint8Array(im.pixels), b64(fld.b64));
        } else if (fld.kind === "bin") {
          assert.deepEqual(new Uint8Array(body.values[fld.key]), b64(fld.b64));
        } else {
          assert.deepEqual(body.values[fld.key], fld.value);
        }
      }
    });
    continue;
  }
  test(`XEX1 golden: ${f.file}`, () => {
    const bytes = new Uint8Array(readFileSync(path.join(FIX, f.file)));
    const body = decodeXEX1(bytes);
    assert.ok(body, `decodeXEX1 returned null for ${f.file}`);
    assert.equal(body.v, 1);
    assert.equal(body.channel, f.channel);
    assert.equal(body.seq, f.seq);
    assert.equal(body.json, f.json);

    // images: key order, byte-exact payloads.
    assert.equal(body.images.length, f.images.length);
    for (let i = 0; i < f.images.length; i++) {
      assert.equal(body.images[i].key, f.images[i].key);
      const want = b64(f.images[i].b64);
      assert.equal(body.images[i].jpeg.length, f.images[i].size);
      assert.deepEqual(new Uint8Array(body.images[i].jpeg), want);
    }

    // forward-compatible extension keys (map16 frame) survive decode.
    for (const k of f.extra_keys) assert.ok(k in body, `missing extra key ${k}`);
  });
}

test("XEX1 non-finite sentinels restore to real numbers", () => {
  const bytes = new Uint8Array(readFileSync(path.join(FIX, "nonfinite.bin")));
  const values = restoreNonFinite(JSON.parse(decodeXEX1(bytes).json));
  assert.ok(Number.isNaN(values.a));
  assert.equal(values.b, Infinity);
  assert.equal(values.c, -Infinity);
  assert.equal(values.d, 1.5);
});

// Regression guard: the map16 golden (>15 top-level keys) is exactly the frame a
// fixmap-only decoder could not read. Prove the golden exercises the cap by
// decoding it with a decoder stripped of the map16 opcode — it must throw — while
// the shipped decoder (above) reads it fine.
test("map16 golden fails a fixmap-only decoder (the cap the fix closes)", () => {
  const map16 = new Uint8Array(readFileSync(path.join(FIX, "map16.bin")));
  assert.equal(map16[4], 0xde, "map16.bin must start (post-magic) with the map16 opcode");

  const html = readFileSync(HTML, "utf8");
  const region = html.match(/\/\/ <XEX1-DECODER>([\s\S]*?)\/\/ <\/XEX1-DECODER>/)[1];
  const crippled = region.replace(/case 0xde:.*$/m, "").replace(/case 0xdf:.*$/m, "");
  // eslint-disable-next-line no-new-func
  const { decodeXEX1: decodeOld } = new Function(
    crippled + "\nreturn { decodeXEX1 };")();
  // decodeXEX1 swallows the throw and returns null → the frame is unreadable.
  assert.equal(decodeOld(map16), null);
});
