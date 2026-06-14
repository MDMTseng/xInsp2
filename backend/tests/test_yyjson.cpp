// test_yyjson.cpp — yyjson foundation: build (mut) -> write JSON -> read, and
// the pool-allocator path (zero per-frame allocation). Validates yyjson works in
// our build before it replaces cJSON as the Record DOM.
#include "yyjson.h"

#include <cstdio>
#include <cstring>
#include <string>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } } while (0)

// Build a matcher-like doc with the given allocator (NULL = default).
static yyjson_mut_doc* build(yyjson_alc* alc) {
    yyjson_mut_doc* doc = yyjson_mut_doc_new(alc);
    yyjson_mut_val* root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "$src", "matcher");
    yyjson_mut_obj_add_int(doc, root, "match_count", 3);
    yyjson_mut_val* arr = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, root, "matches", arr);  // incremental, no count needed
    for (int i = 0; i < 3; ++i) {
        yyjson_mut_val* m = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, m, "name", "part");
        yyjson_mut_obj_add_real(doc, m, "x", 100.0 + i * 1.5);
        yyjson_mut_obj_add_real(doc, m, "score", 0.7 + i * 0.01);
        yyjson_mut_obj_add_bool(doc, m, "flipped", i & 1);
        yyjson_mut_arr_add_val(arr, m);
    }
    return doc;
}

static void verify(yyjson_doc* rd, const char* tag) {
    yyjson_val* root = yyjson_doc_get_root(rd);
    CHECK(root && yyjson_is_obj(root), tag);
    CHECK(std::strcmp(yyjson_get_str(yyjson_obj_get(root, "$src")), "matcher") == 0, "read $src");
    CHECK(yyjson_get_int(yyjson_obj_get(root, "match_count")) == 3, "read match_count");
    yyjson_val* arr = yyjson_obj_get(root, "matches");
    CHECK(yyjson_arr_size(arr) == 3, "read matches size");
    yyjson_val* m1 = yyjson_arr_get(arr, 1);
    CHECK(yyjson_get_real(yyjson_obj_get(m1, "x")) == 101.5, "read matches[1].x");
    CHECK(yyjson_get_bool(yyjson_obj_get(m1, "flipped")) == true, "read matches[1].flipped");
}

int main() {
    // 1) default allocator: build -> write -> read -> verify
    {
        yyjson_mut_doc* doc = build(nullptr);
        size_t len = 0;
        char* json = yyjson_mut_write(doc, 0, &len);
        CHECK(json && len > 0, "mut_write produced JSON");
        yyjson_doc* rd = yyjson_read(json, len, 0);
        verify(rd, "default-alloc round-trip");
        yyjson_doc_free(rd);
        free(json);
        yyjson_mut_doc_free(doc);
    }

    // 2) POOL allocator: a fixed thread-local buffer — zero heap allocation for
    //    the whole build + write (the per-frame hot-path pattern).
    {
        static char pool[1 << 16];   // 64 KiB scratch, reused each "frame"
        yyjson_alc alc;
        bool ok = yyjson_alc_pool_init(&alc, pool, sizeof(pool));
        CHECK(ok, "pool alc init");

        yyjson_mut_doc* doc = build(&alc);            // builds from the pool
        size_t len = 0;
        yyjson_write_err werr;
        char* json = yyjson_mut_write_opts(doc, 0, &alc, &len, &werr);  // writes into the pool
        CHECK(json && len > 0, "pool mut_write produced JSON");

        // read can use the same pool (separate region) or default; use pool buffer #2
        static char pool2[1 << 16];
        yyjson_alc ralc; yyjson_alc_pool_init(&ralc, pool2, sizeof(pool2));
        yyjson_doc* rd = yyjson_read_opts(json, len, 0, &ralc, nullptr);
        verify(rd, "pool-alloc round-trip");
        // no frees needed — everything lives in the reusable pools
    }

    if (fails) { std::printf("%d FAIL(s)\n", fails); return 1; }
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
