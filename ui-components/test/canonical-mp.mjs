// canonical-mp.mjs test — the canonical max-width msgpack profile
// (src/canonical-mp.mjs). Mirrors the Python suite
// (tools/xinsp2_py/tests/test_canonical.py): profile invariants, determinism,
// float canonicalization, hostile-input rejection, the ingress recanonicalize
// pass, and the SHARED cross-language vectors in contract/canonical-vectors.json
// — the same file the Python suite checks. Byte-for-byte agreement on those
// vectors is the cross-language validation of the profile.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

import {
  encodeCanonical, decode, recanonicalize,
  f64, Ext,
  CanonicalError, EncodeError, IntRangeError,
  DecodeError, TruncatedError, TrailingBytesError,
  DepthLimitError, MapKeyError, ExtNotAllowedError,
} from "../src/canonical-mp.mjs";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, "..", "..");
const VECTORS = JSON.parse(readFileSync(path.join(REPO, "contract", "canonical-vectors.json"), "utf8"));

const hex = (u8) => Buffer.from(u8).toString("hex");
const bytes = (h) => new Uint8Array(Buffer.from(h, "hex"));

const INT64_MAX = 2n ** 63n - 1n;
const INT64_MIN = -(2n ** 63n);
const UINT64_MAX = 2n ** 64n - 1n;

// ---- profile invariants: everything is max width --------------------------

test("int always uses the int64 marker (no compaction)", () => {
  for (const v of [0, 1, -1, 127, 128, 255, 256, 65535, 65536n, INT64_MAX, INT64_MIN]) {
    const out = encodeCanonical(v);
    assert.equal(out.length, 9, `${v} should be marker + 8 bytes`);
    assert.equal(out[0], 0xd3, `${v} must use int64 marker`);
  }
});

test("int beyond int64 uses uint64", () => {
  for (const v of [INT64_MAX + 1n, UINT64_MAX]) {
    const out = encodeCanonical(v);
    assert.equal(out[0], 0xcf);
    assert.equal(out.length, 9);
  }
});

test("int out of range is rejected", () => {
  assert.throws(() => encodeCanonical(UINT64_MAX + 1n), IntRangeError);
  assert.throws(() => encodeCanonical(INT64_MIN - 1n), IntRangeError);
});

test("float always float64", () => {
  for (const v of [1.5, -2.5, 0.1, f64(0), f64(1)]) {
    const out = encodeCanonical(v);
    assert.equal(out[0], 0xcb);
    assert.equal(out.length, 9);
  }
});

test("str always str32, bin always bin32", () => {
  assert.equal(encodeCanonical("hi")[0], 0xdb);
  assert.equal(hex(encodeCanonical("hi")).slice(0, 10), "db00000002");
  assert.equal(encodeCanonical(new Uint8Array([1, 2]))[0], 0xc6);
});

test("containers always widest (array32/map32)", () => {
  assert.equal(encodeCanonical([1, 2])[0], 0xdd);
  assert.equal(encodeCanonical({ a: 1 })[0], 0xdf);
  assert.equal(encodeCanonical(new Map([["a", 1]]))[0], 0xdf);
});

// ---- number policy ---------------------------------------------------------

test("integer-valued number encodes as int64; f64() forces float64", () => {
  assert.equal(encodeCanonical(5)[0], 0xd3);
  assert.equal(encodeCanonical(f64(5))[0], 0xcb);
  assert.equal(encodeCanonical(1.5)[0], 0xcb);        // non-integer number -> float
});

test("bigint always integer; matches number for small values", () => {
  assert.deepEqual(encodeCanonical(5n), encodeCanonical(5));
});

// ---- determinism -----------------------------------------------------------

test("encode is deterministic", () => {
  const obj = { b: 2, a: [1, 2, { x: true }], c: "str" };
  assert.deepEqual(encodeCanonical(obj), encodeCanonical(obj));
});

test("map key order is insertion order, not sorted", () => {
  const a = encodeCanonical({ z: 1, a: 2 });
  const b = encodeCanonical({ a: 2, z: 1 });
  assert.notDeepEqual(a, b);
});

test("bool not confused with int", () => {
  assert.equal(hex(encodeCanonical(true)), "c3");
  assert.equal(hex(encodeCanonical(false)), "c2");
});

// ---- float canonicalization ------------------------------------------------

test("NaN normalized to the canonical pattern", () => {
  const canonical = "cb7ff8000000000000";
  // build assorted NaN bit patterns and force them onto the float wire.
  for (const bitsHex of ["7ff8000000000000", "fff8000000000001", "7ff0000000000001"]) {
    const dv = new DataView(new ArrayBuffer(8));
    for (let i = 0; i < 8; i++) dv.setUint8(i, parseInt(bitsHex.slice(i * 2, i * 2 + 2), 16));
    const num = dv.getFloat64(0, false);
    assert.equal(hex(encodeCanonical(f64(num))), canonical, `NaN ${bitsHex}`);
  }
});

test("negative zero preserved, distinct from positive zero", () => {
  assert.equal(hex(encodeCanonical(f64(-0))), "cb8000000000000000");
  assert.equal(hex(encodeCanonical(f64(0))), "cb0000000000000000");
});

test("infinities preserved", () => {
  assert.equal(hex(encodeCanonical(Infinity)), "cb7ff0000000000000");
  assert.equal(hex(encodeCanonical(-Infinity)), "cbfff0000000000000");
});

// ---- round trips -----------------------------------------------------------

test("round trip of assorted values", () => {
  const cases = [null, true, false, 0, -1, 42, 1.5, "", "héllo", [], [1, "a", null], {}, { k: "v", n: [1, 2] }];
  for (const v of cases) {
    assert.deepEqual(decode(encodeCanonical(v)), v, JSON.stringify(v));
  }
});

test("int64 boundary values round trip via BigInt", () => {
  assert.equal(decode(encodeCanonical(INT64_MAX)), INT64_MAX);      // beyond safe int -> BigInt
  assert.equal(decode(encodeCanonical(INT64_MIN)), INT64_MIN);
  assert.equal(decode(encodeCanonical(UINT64_MAX)), UINT64_MAX);
});

test("small ints decode to number, large to BigInt (number policy)", () => {
  assert.equal(typeof decode(encodeCanonical(5)), "number");
  assert.equal(typeof decode(encodeCanonical(UINT64_MAX)), "bigint");
});

test("bin decodes to Uint8Array", () => {
  const got = decode(encodeCanonical(new Uint8Array([0, 255, 7])));
  assert.ok(got instanceof Uint8Array);
  assert.deepEqual([...got], [0, 255, 7]);
});

// ---- decoder accepts compact / standard forms ------------------------------

test("decode reads compact forms", () => {
  assert.equal(decode(bytes("05")), 5);
  assert.equal(decode(bytes("fb")), -5);
  assert.equal(decode(bytes("ccc8")), 200);
  assert.equal(decode(bytes("ca3fc00000")), 1.5);      // float32 widened
  assert.equal(decode(bytes("a568656c6c6f")), "hello");
});

// ---- hostile inputs are rejected -------------------------------------------

test("truncated scalar rejected", () => {
  assert.throws(() => decode(bytes("d300000000")), TruncatedError);
});

test("truncated str length rejected", () => {
  assert.throws(() => decode(bytes("db000003e8")), TruncatedError);
});

test("array length lie rejected before alloc", () => {
  assert.throws(() => decode(bytes("ddffffffff")), TruncatedError);
});

test("map length lie rejected before alloc", () => {
  assert.throws(() => decode(bytes("dfffffffff")), TruncatedError);
});

test("depth bomb rejected", () => {
  const payload = new Uint8Array([...new Array(200).fill(0x91), 0x00]);
  assert.throws(() => decode(payload), DepthLimitError);
});

test("depth limit configurable", () => {
  const payload = new Uint8Array([0x91, 0x91, 0x91, 0x00]);   // [[[0]]]
  assert.deepEqual(decode(payload, { maxDepth: 64 }), [[[0]]]);
  assert.throws(() => decode(payload, { maxDepth: 2 }), DepthLimitError);
});

test("trailing bytes rejected", () => {
  const withTrailer = new Uint8Array([...encodeCanonical(1), 0x00]);
  assert.throws(() => decode(withTrailer), TrailingBytesError);
});

test("invalid byte 0xc1 rejected", () => {
  assert.throws(() => decode(bytes("c1")), DecodeError);
});

test("non-string map key rejected", () => {
  assert.throws(() => decode(bytes("810102")), MapKeyError);   // {1: 2}
});

// ---- ext handling ----------------------------------------------------------

test("ext rejected by default", () => {
  assert.throws(() => decode(bytes("d407aa")), ExtNotAllowedError);
});

test("ext allowed when whitelisted", () => {
  const got = decode(bytes("d407aa"), { allowExt: [7] });
  assert.ok(got instanceof Ext);
  assert.equal(got.code, 7);
  assert.deepEqual([...got.data], [0xaa]);
});

test("encoder refuses to emit ext", () => {
  assert.throws(() => encodeCanonical(new Ext(7, new Uint8Array([0xaa]))), EncodeError);
});

test("recanonicalize refuses ext by default", () => {
  assert.throws(() => recanonicalize(bytes("d407aa")), ExtNotAllowedError);
});

// ---- recanonicalize (ingress) ----------------------------------------------

test("recanonicalize widens compact", () => {
  assert.equal(hex(recanonicalize(bytes("05"))), "d30000000000000005");
});

test("recanonicalize is idempotent", () => {
  const once = encodeCanonical({ w: 640, h: 480, roi: [1, 2, 3, 4], scale: 1.5, ok: true });
  assert.deepEqual(recanonicalize(once), once);
});

test("recanonicalize normalizes NaN", () => {
  assert.equal(hex(recanonicalize(bytes("cbfff8000000000001"))), "cb7ff8000000000000");
});

test("recanonicalize preserves int-vs-float through transcode", () => {
  // float32 1.5 stays a float64; fixint 5 stays an int64 — the distinction JS
  // would lose via decode->encode is preserved by the byte-level transcoder.
  assert.equal(hex(recanonicalize(bytes("ca3fc00000"))), "cb3ff8000000000000");
  assert.equal(hex(recanonicalize(bytes("05"))), "d30000000000000005");
});

test("recanonicalize validates", () => {
  assert.throws(() => recanonicalize(bytes("ddffffffff")), TruncatedError);
});

// ---- shared cross-language vectors -----------------------------------------

function build(node) {
  switch (node.t) {
    case "null": return null;
    case "bool": return node.v;
    case "int": return typeof node.v === "string" ? BigInt(node.v) : node.v;
    case "f64": {
      if ("bits" in node) {
        const dv = new DataView(new ArrayBuffer(8));
        for (let i = 0; i < 8; i++) dv.setUint8(i, parseInt(node.bits.slice(i * 2, i * 2 + 2), 16));
        return f64(dv.getFloat64(0, false));       // wrap: force float even for integer-valued
      }
      return f64(node.v);
    }
    case "str": return node.v;
    case "bin": return bytes(node.hex);
    case "array": return node.v.map(build);
    case "map": {
      const o = {};
      for (const [k, x] of node.v) o[k] = build(x);
      return o;
    }
    default: throw new Error(`bad node type ${node.t}`);
  }
}

for (const c of VECTORS.encode) {
  test(`shared encode vector: ${c.name}`, () => {
    assert.equal(hex(encodeCanonical(build(c.value))), c.hex);
  });
}

for (const c of VECTORS.recanon) {
  test(`shared recanon vector: ${c.name}`, () => {
    assert.equal(hex(recanonicalize(bytes(c.in_hex))), c.out_hex);
  });
}

// ---- cross-check against the C++ golden fixtures ---------------------------
// The Node leg of the three-way validation: the C++ Writer/canonicalizer emits
// the golden .bin files under protocol/fixtures/canonical/, and this codec
// consumes the SAME bytes. A canonical golden must recanonicalize to itself; the
// compact golden must recanonicalize to its canonical sibling; every hostile
// golden must be rejected. Mirrors the Python leg in test_canonical.py.
const CANON_DIR = path.join(REPO, "protocol", "fixtures", "canonical");
let MANIFEST = null;
try {
  MANIFEST = JSON.parse(readFileSync(path.join(CANON_DIR, "manifest.json"), "utf8"));
} catch {
  MANIFEST = null;   // goldens not generated on this checkout — skip the leg
}

if (MANIFEST) {
  for (const entry of MANIFEST.canonical) {
    test(`C++ golden recanonicalizes to itself: ${entry.file}`, () => {
      const raw = new Uint8Array(readFileSync(path.join(CANON_DIR, entry.file)));
      assert.deepEqual(recanonicalize(raw), raw);
    });
  }

  test("C++ compact golden recanonicalizes to its canonical sibling", () => {
    const cp = MANIFEST.compact_pair;
    const compact = new Uint8Array(readFileSync(path.join(CANON_DIR, cp.compact_file)));
    const canonical = new Uint8Array(readFileSync(path.join(CANON_DIR, cp.canonical_file)));
    assert.deepEqual(recanonicalize(compact), canonical);
  });

  for (const entry of MANIFEST.hostile) {
    test(`C++ hostile golden rejected: ${entry.file}`, () => {
      const raw = new Uint8Array(readFileSync(path.join(CANON_DIR, entry.file)));
      assert.throws(() => recanonicalize(raw), CanonicalError);
    });
  }
}
