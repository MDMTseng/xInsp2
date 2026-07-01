// fuzz_yyjson.cpp — coverage-guided fuzz target for the raw yyjson decode path.
//
// Boundary under test: yyjson_read() — the vendored parser that backs
// xi::Record::from_json_bytes() (xi_record.hpp) and hence EVERY wire/ABI record
// decode. This harness deliberately hits yyjson directly (no OpenCV / Record
// materialisation) so it is lean, links only the vendored yyjson.c, and isolates
// any finding to the parser itself rather than the Record wrapper.
//
// We exercise the same read flags the record path uses (default 0) plus the more
// permissive flags the codebase can reach, then walk the resulting DOM (root
// type, object/array iteration, number/string extraction) to drive coverage into
// the value-materialisation code, and round-trip through the writer.
//
// yyjson.c is vendored third-party (v0.12.0); a crash here is an upstream/vendor
// finding — report, do not patch core.
//
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "yyjson.h"

static void walk(yyjson_val* v, int depth) {
    if (!v || depth > 64) return;
    switch (yyjson_get_type(v)) {
        case YYJSON_TYPE_ARR: {
            size_t idx, max;
            yyjson_val* item;
            yyjson_arr_foreach(v, idx, max, item) { walk(item, depth + 1); }
            break;
        }
        case YYJSON_TYPE_OBJ: {
            size_t idx, max;
            yyjson_val *key, *val;
            yyjson_obj_foreach(v, idx, max, key, val) {
                (void)yyjson_get_str(key);
                walk(val, depth + 1);
            }
            break;
        }
        case YYJSON_TYPE_STR: {
            volatile const char* s = yyjson_get_str(v);
            (void)s;
            volatile size_t n = yyjson_get_len(v);
            (void)n;
            break;
        }
        case YYJSON_TYPE_NUM: {
            volatile double d = yyjson_get_real(v);
            (void)d;
            volatile int64_t i = yyjson_get_sint(v);
            (void)i;
            break;
        }
        default:
            break;
    }
}

static void run(const char* data, size_t size, yyjson_read_flag flag) {
    yyjson_doc* doc = yyjson_read(data, size, flag);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);
    walk(root, 0);
    // Round-trip: writer path is part of Record::data_json().
    size_t len = 0;
    char* out = yyjson_write(doc, 0, &len);
    if (out) free(out);
    yyjson_doc_free(doc);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* p = reinterpret_cast<const char*>(data);
    // Exactly the flag the record decode uses.
    run(p, size, 0);
    // And the permissive superset (comments/trailing/inf-nan) to widen coverage.
    run(p, size,
        YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_COMMENTS |
        YYJSON_READ_ALLOW_INF_AND_NAN | YYJSON_READ_ALLOW_INVALID_UNICODE);
    return 0;
}
