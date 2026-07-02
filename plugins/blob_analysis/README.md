# blob_analysis

Threshold + connected-component analysis. Given a grayscale image it produces a
binary image and, per blob, area / centroid / bounding box / contour.

This plugin implements the **plugin data contract**
([docs/new_gen/02-plugin-data-contract.md](../../docs/new_gen/02-plugin-data-contract.md)):
a typed builder/extractor over a schemaless Record, fail-loud required inputs,
and a schema-version stamp. Nothing new crosses the ABI — the wrappers compile
down to plain `Record` `set`/`get`.

## Keys — one source of truth

Every Record key this plugin reads or writes is named **once** in
[`blob_analysis_keys.h`](./blob_analysis_keys.h). The plugin's own reader, the
input builder, and the output extractor all compile from those constants, so a
key rename can't drift. Do not hard-code the string names below — include the
header and use the typed view.

| Direction | Key | Type | Notes |
|-----------|-----|------|-------|
| input  | `gray`      | image (1-ch) | **required** |
| input  | `threshold` | int  | optional, default 128 |
| input  | `min_area`  | int  | optional, default 10 |
| input  | `max_area`  | int  | optional, default 999999 |
| input  | `invert`    | bool | optional, default false |
| output | `binary`         | image | thresholded (pool-backed, zero-copy) |
| output | `blob_count`     | int   | |
| output | `threshold_used` | int   | |
| output | `blobs`          | array | per-blob: `area`, `cx`, `cy`, `min_x/y`, `max_x/y`, `contour_points`, `contour[]` |

Schema version: `xi::blob::kSchemaVersion` (currently **1**). Bump it on an
incompatible input change; a script compiled against a different version is
rejected with a precise error naming both versions.

## Using it from a script

Include [`blob_analysis_io.h`](./blob_analysis_io.h) and never touch a raw
string key — a typo becomes a compile error, and the schema stamp lets the
plugin report a header/plugin skew precisely:

```cpp
#include <xi/xi_result.hpp>   // xi::ok / xi::ng / xi::result
#include "blob_analysis_io.h"

XI_INSPECT_ENTRY(t, frame) {
    xi::Image gray = /* ... a single-channel image ... */;

    // Build the input with typed setters (stamps the schema version).
    xi::Record in = xi::blob::Input()
        .gray(gray)
        .threshold(90)
        .min_area(25);

    // Run the "det0" instance and read the output with the typed extractor.
    xi::blob::Output out{ xi::use("det0").process(in) };

    // Fail-loud: a missing/mis-typed required input (or a schema skew) comes
    // back as a structured NA the script maps to a verdict — never a silent 0.
    // (Only the script knows whether det0 is on the critical path.)
    if (!out.ok()) {
        xi::result(0, out.na_reason());   // code 0 = NA, with the fault reason
        return;
    }

    int total = 0;
    for (int i = 0; i < out.blob_count(); ++i)
        total += out.blob(i).area();
    xi::ok(1, "blobs=" + std::to_string(out.blob_count()));   // surface a verdict
}
```

## Failure shape

A required input that is missing/mis-typed, or a schema skew, returns a
structured failure Record (see `xi::contract`):

```json
{ "$na": "missing required input 'gray' (expected image)",
  "$fault": { "code": "missing_input", "key": "gray", "expected_type": "image" } }
```

`code` is one of `missing_input`, `wrong_type`, `schema_mismatch`. Unknown extra
input keys are ignored (schemaless tolerance) and warned once.

## Tests

`tests/test_blob_analysis.cpp` drives the built DLL through the host ImagePool
and asserts: missing `gray` → structured `missing_input`; a future `_schema` →
`schema_mismatch` naming both versions; and the builder/extractor happy path.
Run via `ctest -C Release -R blob_analysis_test` from `plugins/build`.
