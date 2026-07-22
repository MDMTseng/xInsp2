// options.mjs test — the pure options normalizer for xi-radio / xi-dropdown.
import { test } from "node:test";
import assert from "node:assert/strict";
import { parseOptions } from "./options.mjs";

test("parseOptions handles arrays, JSON strings, and {value,label}", () => {
  assert.deepEqual(parseOptions(["a", "b"]), [
    { value: "a", label: "a" }, { value: "b", label: "b" },
  ]);
  assert.deepEqual(parseOptions('["x","y"]'), [
    { value: "x", label: "x" }, { value: "y", label: "y" },
  ]);
  assert.deepEqual(parseOptions([{ value: 1, label: "One" }, { value: 2 }]), [
    { value: 1, label: "One" }, { value: 2, label: "2" },
  ]);
  assert.deepEqual(parseOptions("not json"), []);
  assert.deepEqual(parseOptions(undefined), []);
  assert.deepEqual(parseOptions(null), []);
});
