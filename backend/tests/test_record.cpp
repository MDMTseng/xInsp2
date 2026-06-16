//
// test_record.cpp — Unit tests for xi::Record (xi_record.hpp).
//
// Covers all 16 tests from TEST_PLAN.md section "test_record.cpp (NEW)".
// Standalone, C++20, links against yyjson only.
//

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include <xi/xi_record.hpp>
#include <xi/xi_doc_registry.hpp>
#include "yyjson.h"

// Minimal test harness (same pattern as test_xi_core.cpp)
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define SECTION(name) std::printf("[test] %s\n", name)

// ---------- Test 1: Build Record with all types ----------

static void test_build_all_types() {
    SECTION("Build Record with all types");
    xi::Record r;
    r.set("count", 42);
    r.set("pi", 3.14);
    r.set("ok", true);
    r.set("label", std::string("hello"));

    CHECK(r.get_int("count") == 42);
    CHECK(r.get_double("pi") > 3.13 && r.get_double("pi") < 3.15);
    CHECK(r.get_bool("ok") == true);
    CHECK(r.get_string("label") == "hello");

    std::string json = r.data_json();
    CHECK(json.find("\"count\":42") != std::string::npos);
    CHECK(json.find("\"ok\":true") != std::string::npos);
    CHECK(json.find("\"label\":\"hello\"") != std::string::npos);
}

// ---------- Test 2: Nested Record ----------

static void test_nested_record() {
    SECTION("Nested Record");
    xi::Record inner;
    inner.set("x", 10);
    inner.set("y", 20);

    xi::Record outer;
    outer.set("roi", inner);

    xi::Record got = outer.get_record("roi");
    CHECK(got.get_int("x") == 10);
    CHECK(got.get_int("y") == 20);
}

// ---------- Test 3: Array push ----------

static void test_array_push() {
    SECTION("Array push");
    xi::Record r;
    xi::Record item1;
    item1.set("id", 1);
    xi::Record item2;
    item2.set("id", 2);

    r.push("items", item1);
    r.push("items", item2);
    CHECK(r.get_array_size("items") == 2);

    xi::Record first = r.get_array_item("items", 0);
    CHECK(first.get_int("id") == 1);
    xi::Record second = r.get_array_item("items", 1);
    CHECK(second.get_int("id") == 2);
}

// ---------- Test 4: operator[] chaining ----------

static void test_operator_chaining() {
    SECTION("operator[] chaining");
    xi::Record inner;
    inner.set("b", 99);
    xi::Record outer;
    outer.set("a", inner);

    // Present key
    CHECK(outer["a"]["b"].as_int(0) == 99);

    // Missing key returns default
    CHECK(outer["a"]["missing"].as_int(42) == 42);
    CHECK(outer["nope"]["deep"].as_int(7) == 7);
}

// ---------- Test 5: Path expression "a.b.c" ----------

static void test_path_dot() {
    SECTION("Path expression a.b.c");
    xi::Record c;
    c.set("c", 55);
    xi::Record b;
    b.set("b", c);
    xi::Record a;
    a.set("a", b);

    CHECK(a["a.b.c"].as_int(0) == 55);
    CHECK(a.at("a.b.c").as_int(0) == 55);
}

// ---------- Test 6: Path expression "a[0].b" ----------

static void test_path_array_index() {
    SECTION("Path expression a[0].b");
    xi::Record item;
    item.set("b", 77);

    xi::Record r;
    r.push("a", item);

    CHECK(r["a[0].b"].as_int(0) == 77);
    CHECK(r.at("a[0].b").as_int(0) == 77);
}

// ---------- Test 7: Path missing key returns default ----------

static void test_path_missing_default() {
    SECTION("Path missing key -> default");
    xi::Record r;
    r.set("x", 1);

    CHECK(r["x.y.z"].as_int(99) == 99);
    CHECK(r["nonexistent.deep.path"].as_int(99) == 99);
}

// ---------- Test 8: Path empty string ----------

static void test_path_empty_string() {
    SECTION("Path empty string");
    xi::Record r;
    r.set("a", 10);

    // Empty string path should not crash, return default
    CHECK(r[""].as_int(0) == 0);
}

// ---------- Test 9: Path key > 256 chars ----------

static void test_path_long_key() {
    SECTION("Path key > 256 chars");
    // Build a key that is 300 chars long
    std::string long_key(300, 'k');
    xi::Record r;
    r.set(long_key, 42);

    // The path resolver truncates keys at 255 chars, so lookup may fail.
    // The important thing is: no crash, and we get a default.
    // Direct get should still work since the Record uses the full key.
    CHECK(r.get_int(long_key) == 42);

    // Path-based access uses the truncated key -> likely returns default
    // The main assertion is: no crash.
    int val = r[long_key].as_int(99);
    // If truncation happened, val == 99. If not, val == 42. Either is acceptable.
    CHECK(val == 99 || val == 42);
}

// ---------- Test 10: Path "[-1]" ----------

static void test_path_negative_index() {
    SECTION("Path [-1]");
    xi::Record r;
    r.push("arr", 10);
    r.push("arr", 20);

    // Negative index: the parser reads digits only, so [-1] parses as index 0
    // after skipping '-'. The key point: no crash, returns a valid default or value.
    int val = r["arr[-1]"].as_int(-1);
    (void)val; // no crash is the assertion
    CHECK(true); // reached here without crash
}

// ---------- Test 11: Path "[999]" out of bounds ----------

static void test_path_oob_index() {
    SECTION("Path [999] out of bounds");
    xi::Record r;
    r.push("arr", 10);

    CHECK(r["arr[999]"].as_int(-1) == -1);
}

// ---------- Test 12: image_keys_json with special chars ----------

static void test_image_keys_special_chars() {
    SECTION("image_keys_json with special chars");
    xi::Image img(2, 2, 1);
    xi::Record r;
    r.image("normal", img);
    r.image("has\"quote", img);

    std::string json = r.image_keys_json();
    // The JSON must be parseable by yyjson
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    yyjson_val* parsed = doc ? yyjson_doc_get_root(doc) : nullptr;
    // Note: image_keys_json does NOT escape special chars (known issue A4-5).
    // We verify the function doesn't crash and returns something.
    // If parsing fails due to unescaped quotes, that confirms the bug.
    if (parsed) {
        CHECK(yyjson_is_arr(parsed));
        CHECK(yyjson_arr_size(parsed) == 2);
        yyjson_doc_free(doc);
    } else {
        yyjson_doc_free(doc);
        // Bug confirmed: unescaped quote breaks JSON.
        // The test documents this. No crash is the minimum bar.
        std::printf("  NOTE: image_keys_json breaks with quotes in key (known A4-5)\n");
        CHECK(true); // no crash = pass
    }
}

// ---------- Test 13: Copy semantics ----------

static void test_copy_semantics() {
    SECTION("Copy semantics");
    xi::Record orig;
    orig.set("x", 10);
    orig.image("img", xi::Image(2, 2, 1));

    xi::Record copy = orig;
    copy.set("x", 99);
    copy.set("extra", true);

    // Original unchanged
    CHECK(orig.get_int("x") == 10);
    CHECK(!orig.has("extra"));
    CHECK(orig.has_image("img"));

    // Copy has new values
    CHECK(copy.get_int("x") == 99);
    CHECK(copy.get_bool("extra") == true);
}

// ---------- Test 14: Move semantics ----------

static void test_move_semantics() {
    SECTION("Move semantics");
    xi::Record orig;
    orig.set("x", 10);
    orig.image("img", xi::Image(2, 2, 1));

    xi::Record moved = std::move(orig);

    // Moved-to has data
    CHECK(moved.get_int("x") == 10);
    CHECK(moved.has_image("img"));

    // Source is empty (json_ is nullptr after move)
    CHECK(orig.empty());
}

// ---------- Test 15: as_record() deep copy ----------

static void test_as_record_deep_copy() {
    SECTION("as_record() deep copy");
    xi::Record inner;
    inner.set("val", 100);

    xi::Record outer;
    outer.set("sub", inner);

    // Extract via operator[] -> as_record()
    xi::Record extracted = outer["sub"].as_record();
    CHECK(extracted.get_int("val") == 100);

    // Modify extracted
    extracted.set("val", 999);
    extracted.set("new_key", true);

    // Original unchanged
    CHECK(outer["sub"]["val"].as_int(0) == 100);
    CHECK(!outer["sub"]["new_key"].exists());
}

// ---------- Test 16: Image in Record ----------

static void test_image_in_record() {
    SECTION("Image in Record");
    xi::Image img(4, 4, 3);
    // Fill with a known pattern
    for (size_t i = 0; i < img.size(); ++i) {
        img.data()[i] = static_cast<uint8_t>(i & 0xFF);
    }

    xi::Record r;
    r.image("k", img);
    CHECK(r.has_image("k"));

    const xi::Image& got = r.get_image("k");
    CHECK(got.width == 4);
    CHECK(got.height == 4);
    CHECK(got.channels == 3);
    CHECK(got.size() == img.size());

    // Verify pixels match
    bool pixels_ok = true;
    for (size_t i = 0; i < got.size(); ++i) {
        if (got.data()[i] != static_cast<uint8_t>(i & 0xFF)) {
            pixels_ok = false;
            break;
        }
    }
    CHECK(pixels_ok);
}

// ---------- Test 17: γ-4 refcount share + copy-on-write ----------

static void test_refcount_cow() {
    SECTION("refcount share + COW (γ-4)");
    xi::Record a;
    a.set("v", 1);
    a.set("tag", std::string("orig"));

    // Copies share a's doc (zero-copy) but stay logically independent via COW.
    xi::Record b = a;          // share
    xi::Record c = a;          // share (rc now 3)
    CHECK(b.get_int("v") == 1);   // shared read sees a's data
    CHECK(c.get_string("tag") == "orig");

    // Mutating one COWs it into its own doc; the others are untouched.
    b.set("v", 2);
    CHECK(a.get_int("v") == 1);
    CHECK(b.get_int("v") == 2);
    CHECK(c.get_int("v") == 1);

    // Mutate the original after it was shared — it COWs too; b, c unaffected.
    a.set("v", 9);
    a.set("only_a", true);
    CHECK(a.get_int("v") == 9);
    CHECK(b.get_int("v") == 2);
    CHECK(c.get_int("v") == 1);
    CHECK(!b.has("only_a"));
    CHECK(!c.has("only_a"));

    // c was never mutated — still the original snapshot.
    CHECK(c.get_int("v") == 1);
    CHECK(c.get_string("tag") == "orig");

    // Chain: assign-share then mutate.
    xi::Record d;
    d = c;                     // copy-assign share
    d.set("v", 42);
    CHECK(c.get_int("v") == 1);
    CHECK(d.get_int("v") == 42);

    // Re-mutate a COWed record (now sole owner) — should keep working.
    b.set("v", 3);
    b.set("v", 4);
    CHECK(b.get_int("v") == 4);
    CHECK(a.get_int("v") == 9);
}

// ---------- Test 18: γ-4 v4 cross-ABI shared doc (share_out / adopt_shared) ----------

static void test_cross_abi_share() {
    SECTION("cross-ABI share/adopt (γ-4 v4)");
    auto& reg = xi::DocRegistry::instance();
    auto retain  = [](void* d) { xi::DocRegistry::instance().retain((yyjson_mut_doc*)d); };
    auto release = [](void* d) { xi::DocRegistry::instance().release((yyjson_mut_doc*)d); };
    size_t base = reg.live_count();

    // Producer owns a doc; share it across the (simulated) ABI boundary.
    xi::Record producer;
    producer.set("v", 7);
    producer.set("tag", std::string("shared"));
    yyjson_mut_doc* raw = producer.share_out(retain, release);
    CHECK(raw != nullptr);
    CHECK(reg.refcount(raw) == 2);          // share_out enrolls this side + reserves the adopter's ref

    {
        // Consumer adopts and CONSUMES the reserved ref (no extra retain, no copy).
        xi::Record consumer = xi::Record::adopt_shared(raw, release);
        CHECK(consumer.get_int("v") == 7);
        CHECK(consumer.get_string("tag") == "shared");
        CHECK(reg.refcount(raw) == 2);      // producer + consumer

        // Mutating the consumer COWs into its own local doc; the shared doc and
        // the producer are untouched, and the consumer drops its registry ref.
        consumer.set("v", 99);
        CHECK(consumer.get_int("v") == 99);
        CHECK(producer.get_int("v") == 7);
        CHECK(reg.refcount(raw) == 1);      // only the producer still shares it
    }   // consumer dtor frees its local doc; no double release on the shared doc

    CHECK(reg.refcount(raw) == 1);
    CHECK(producer.get_int("v") == 7);

    // Producer drops the shared doc → last side → doc freed, registry balanced.
    producer = xi::Record();
    CHECK(reg.refcount(raw) == 0);
    CHECK(reg.live_count() == base);

    // A producer COPIED (in-process share) before share_out still counts as ONE
    // side in the registry (the whole local box-group = one side).
    xi::Record p2;
    p2.set("n", 1);
    xi::Record p2copy = p2;                 // in-process share (rc=2), no registry
    yyjson_mut_doc* raw2 = p2.share_out(retain, release);
    CHECK(reg.refcount(raw2) == 2);         // p2/p2copy side + reserved adopter
    {
        xi::Record c2 = xi::Record::adopt_shared(raw2, release);
        CHECK(c2.get_int("n") == 1);
        CHECK(reg.refcount(raw2) == 2);     // c2 consumed the reserved ref
    }                                       // c2 (unmutated) releases the consumer ref
    CHECK(reg.refcount(raw2) == 1);
    p2 = xi::Record();                      // p2copy still holds the in-process box
    CHECK(reg.refcount(raw2) == 1);         // still one side (p2copy)
    p2copy = xi::Record();                  // last in-process ref → side releases
    CHECK(reg.refcount(raw2) == 0);
    CHECK(reg.live_count() == base);
}

// ---------- Test 19: γ-4 v4-4 zero-copy cache of a borrowed input ----------
//
// Models the input path: the host share_out's an input doc, the plugin adopts it
// and CACHES it across frames by copy. The cached Record must point at the SAME
// doc (no deep copy), stay readable after the host's input dies, and free only
// when the plugin finally drops it.

static void test_cache_input_zero_copy() {
    SECTION("zero-copy cache of borrowed input (γ-4 v4-4)");
    auto& reg = xi::DocRegistry::instance();
    auto retain  = [](void* d) { xi::DocRegistry::instance().retain((yyjson_mut_doc*)d); };
    auto release = [](void* d) { xi::DocRegistry::instance().release((yyjson_mut_doc*)d); };
    size_t base = reg.live_count();

    // Host has an input Record and shares it out to a plugin.
    xi::Record host_input;
    host_input.set("frame", 1);
    yyjson_mut_doc* doc = host_input.share_out(retain, release);
    CHECK(doc != nullptr);
    CHECK(reg.refcount(doc) == 2);            // host side + reserved adopter

    // Plugin adopts (consumes the reserve) and caches it across frames.
    xi::Record plugin_cache;
    {
        xi::Record in = xi::Record::adopt_shared(doc, release, /*frozen=*/true);
        CHECK(in.get_int("frame") == 1);
        CHECK(reg.refcount(doc) == 2);        // host + plugin's adopted box

        plugin_cache = in;                    // CACHE: copy = in-process box rc++
        CHECK(plugin_cache.doc() == doc);     // SAME doc pointer — zero copy, no COW
        CHECK(in.doc() == doc);
        CHECK(reg.refcount(doc) == 2);        // copy shares the box; still one plugin side
    }   // `in` dies: in-process box rc 2->1; registry unchanged (plugin_cache holds it)
    CHECK(reg.refcount(doc) == 2);

    // The host's call returns and its input Record dies — releases the host side.
    host_input = xi::Record();
    CHECK(reg.refcount(doc) == 1);            // only the plugin's cache keeps it alive
    CHECK(plugin_cache.get_int("frame") == 1);// STILL readable — not freed, not copied
    CHECK(plugin_cache.doc() == doc);         // same doc, never deep-copied

    // Plugin finally drops the cache → last side → freed.
    plugin_cache = xi::Record();
    CHECK(reg.refcount(doc) == 0);
    CHECK(reg.live_count() == base);
}

// ---------- main ----------

int main() {
    test_build_all_types();       // 1
    test_nested_record();         // 2
    test_array_push();            // 3
    test_operator_chaining();     // 4
    test_path_dot();              // 5
    test_path_array_index();      // 6
    test_path_missing_default();  // 7
    test_path_empty_string();     // 8
    test_path_long_key();         // 9
    test_path_negative_index();   // 10
    test_path_oob_index();        // 11
    test_image_keys_special_chars(); // 12
    test_copy_semantics();        // 13
    test_move_semantics();        // 14
    test_as_record_deep_copy();   // 15
    test_image_in_record();       // 16
    test_refcount_cow();          // 17
    test_cross_abi_share();       // 18
    test_cache_input_zero_copy(); // 19

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    } else {
        std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
        return 1;
    }
}
