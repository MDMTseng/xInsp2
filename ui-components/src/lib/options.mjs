//
// options.mjs — normalize an `options` prop for xi-radio / xi-dropdown. A custom
// element attribute is always a string, so options may arrive as a JSON string
// (options='["a","b"]') or, when set as a JS property, as a real array. Entries
// may be plain values or {value, label} objects. Returns [{value, label}].
//
export function parseOptions(options) {
  let arr = options;
  if (typeof options === "string") {
    try { arr = JSON.parse(options); } catch { arr = []; }
  }
  if (!Array.isArray(arr)) return [];
  return arr.map((o) =>
    o && typeof o === "object" ? { value: o.value, label: o.label ?? String(o.value) }
                               : { value: o, label: String(o) });
}
