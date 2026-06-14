// test_msgpack.cpp — cJSON <-> MessagePack round-trip (xi_msgpack.hpp / CWPack).
#include "cJSON.h"
#include "xi/xi_msgpack.hpp"

#include <cstdio>
#include <string>

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { std::printf("FAIL: %s\n", msg); ++fails; } } while (0)

static std::string js(cJSON* t) { char* s = cJSON_PrintUnformatted(t); std::string o = s ? s : ""; cJSON_free(s); return o; }
static std::string rt(cJSON* t) {  // encode -> decode -> printed JSON of the decoded tree
    auto bytes = xi::cjson_to_msgpack(t);
    cJSON* dec = xi::msgpack_to_cjson(bytes.data(), bytes.size());
    std::string out = js(dec);
    cJSON_Delete(dec);
    return out;
}

int main() {
    // 1) matcher payload (objects in an array, mixed types, reserved $src key)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "$src", "matcher");
    cJSON_AddNumberToObject(root, "match_count", 3);
    cJSON* arr = cJSON_AddArrayToObject(root, "matches");
    for (int i = 0; i < 3; ++i) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "name", "part");
        cJSON_AddNumberToObject(m, "x", 100.0 + i * 1.5);
        cJSON_AddNumberToObject(m, "score", 0.7 + i * 0.01);
        cJSON_AddBoolToObject(m, "flipped", i & 1);
        cJSON_AddItemToArray(arr, m);
    }
    CHECK(js(root) == rt(root), "matcher payload round-trip");

    // 2) primitives, nesting, null, escaped string, negative + integral numbers
    cJSON* r2 = cJSON_CreateObject();
    cJSON_AddBoolToObject(r2, "t", 1);
    cJSON_AddBoolToObject(r2, "f", 0);
    cJSON_AddNullToObject(r2, "n");
    cJSON_AddStringToObject(r2, "s", "quote\"and\\slash\tand\nnl");
    cJSON_AddNumberToObject(r2, "neg", -12.25);
    cJSON_AddNumberToObject(r2, "int", 42);
    cJSON* sub = cJSON_CreateObject(); cJSON_AddNumberToObject(sub, "a", 0.7); cJSON_AddItemToObject(r2, "sub", sub);
    cJSON* nums = cJSON_CreateArray(); for (int i = 0; i < 4; ++i) cJSON_AddItemToArray(nums, cJSON_CreateNumber(i * 0.25)); cJSON_AddItemToObject(r2, "nums", nums);
    CHECK(js(r2) == rt(r2), "primitives/nested/null/escaped round-trip");

    // 3) empty object
    cJSON* e = cJSON_CreateObject();
    CHECK(js(e) == rt(e), "empty object round-trip");
    auto eb = xi::cjson_to_msgpack(e);
    CHECK(eb.size() == 1 && eb[0] == 0x80, "empty object encodes to one fixmap byte");

    // 4) totality: null/empty/garbage decode -> empty object, never crash
    cJSON* z = xi::msgpack_to_cjson(nullptr, 0); CHECK(js(z) == "{}", "null decode -> {}"); cJSON_Delete(z);
    const uint8_t garbage[] = {0xff, 0x00, 0x12};
    cJSON* g = xi::msgpack_to_cjson(garbage, sizeof(garbage)); CHECK(g && cJSON_IsObject(g), "garbage decode -> object (no crash)"); cJSON_Delete(g);

    // 5) larger array — exercises map16/array headers past the fixarray limit
    cJSON* big = cJSON_CreateObject();
    cJSON* ba = cJSON_AddArrayToObject(big, "a");
    for (int i = 0; i < 100; ++i) cJSON_AddItemToArray(ba, cJSON_CreateNumber(i + 0.5));
    CHECK(js(big) == rt(big), "100-element array round-trip");

    cJSON_Delete(root); cJSON_Delete(r2); cJSON_Delete(e); cJSON_Delete(big);

    if (fails) { std::printf("%d FAIL(s)\n", fails); return 1; }
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
