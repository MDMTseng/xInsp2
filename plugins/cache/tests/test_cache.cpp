//
// test_cache.cpp — developer-side tests for the cache (BufferReplay) plugin's
// data contract (docs/new_gen/02-plugin-data-contract.md).
//
// cache replays WHATEVER record it captured, so the buffered payload is an OPEN
// schema (no typed view over it). Its rich COMMAND surface, its ring CONFIG, and
// the process() capture-ack ARE a fixed vocabulary, and that is what the typed
// view (cache_io.h) and these tests cover:
//
//   1. a command whose required payload is missing → a STRUCTURED fault, not a
//      silent no-op: "replay" without an "index", "set_capacity" without a
//      "value";
//   2. the HAPPY PATH through xi::cache::Config / Command / Capture / Status:
//      capture two records (buffered ack climbs), replay_last re-emits through a
//      real host emit sink, and the ring status reads back correctly;
//   3. a CONFIG schema skew → set_def rejects it.
//
// Unknown commands still fall through to get_def() (a driver "status" idiom the
// buffer_replay demo relies on) — asserted here so that stays true.
//
// Build produces cache_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_contract.hpp>

#include "cache_io.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef CACHE_DLL_PATH
#define CACHE_DLL_PATH "xi-cache.dll"
#endif

struct Syms {
    xi_plugin_create_fn   create   = nullptr;
    xi_plugin_destroy_fn  destroy  = nullptr;
    xi_plugin_process_fn  process  = nullptr;
    xi_plugin_exchange_fn exchange = nullptr;
    xi_plugin_get_def_fn  get_def  = nullptr;
    xi_plugin_set_def_fn  set_def  = nullptr;
};
static HMODULE g_dll = nullptr;
static Syms g_syms;
static xi_host_api g_host = xi::ImagePool::make_host_api();
static int g_emit_count = 0;

static void capture_emit(const char*, xi_trigger_id, const xi_record*, int64_t) {
    ++g_emit_count;
}

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(CACHE_DLL_PATH);
    if (!g_dll) { std::fprintf(stderr, "failed to load %s (err %lu)\n", CACHE_DLL_PATH, GetLastError()); std::exit(2); }
    g_syms.create   = (xi_plugin_create_fn)  GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy  = (xi_plugin_destroy_fn) GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.process  = (xi_plugin_process_fn) GetProcAddress(g_dll, "xi_plugin_process");
    g_syms.exchange = (xi_plugin_exchange_fn)GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def  = (xi_plugin_get_def_fn) GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def  = (xi_plugin_set_def_fn) GetProcAddress(g_dll, "xi_plugin_set_def");
    if (!g_syms.create || !g_syms.destroy || !g_syms.process || !g_syms.exchange || !g_syms.get_def || !g_syms.set_def) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n"); std::exit(2);
    }
    g_host.emit_record = &capture_emit;   // observe replays (make_host_api leaves it null)
}

static std::string send_cmd(void* inst, const std::string& cmd) {
    char buf[512];
    int n = g_syms.exchange(inst, cmd.c_str(), buf, sizeof(buf));
    return (n > 0) ? std::string(buf, n) : std::string();
}
static std::string get_def(void* inst) {
    char buf[512];
    int n = g_syms.get_def(inst, buf, sizeof(buf));
    return (n > 0) ? std::string(buf, n) : std::string();
}

// Capture one record (with an image) into the ring; returns the buffered ack.
static xi::cache::Capture capture_one(void* inst) {
    xi_image_handle h = g_host.image_create(8, 8, 1);
    std::memset(g_host.image_data(h), 200, 64);
    xi_record_image imgs[1] = { { "img", h } };
    std::string meta = "{}";
    xi_record in{};
    in.images = imgs; in.image_count = 1;
    in.data = reinterpret_cast<const uint8_t*>(meta.c_str());
    in.len  = static_cast<int32_t>(meta.size());

    xi_record_out out; xi_record_out_init(&out);
    g_syms.process(inst, &in, &out);
    std::string js;
    if (out.out_doc) {
        size_t n = 0;
        char* s = yyjson_mut_write(reinterpret_cast<yyjson_mut_doc*>(out.out_doc), 0, &n);
        js = s ? std::string(s, n) : "{}";
        if (s) free(s);
    } else {
        js = out.data ? std::string(reinterpret_cast<const char*>(out.data), (size_t)out.len) : "{}";
    }
    xi_record_out_free(&out);
    g_host.image_release(h);
    return xi::cache::Capture{ xi::Record::from_json_bytes(reinterpret_cast<const uint8_t*>(js.data()), js.size()) };
}

// --- Tests -----------------------------------------------------------------

XI_TEST(cache_replay_missing_index_is_structured_fault) {
    load_dll();
    void* inst = g_syms.create(&g_host, "buffer");
    // "replay" with no "index" — must fail loud, not silently do nothing.
    auto j = xi::Json::parse(send_cmd(inst, R"({"command":"replay"})"));
    XI_EXPECT(j["error"].as_string() == xi::contract::kMissingInput);
    XI_EXPECT(j["key"].as_string() == std::string(xi::cache::keys::kIndex));
    g_syms.destroy(inst);
}

XI_TEST(cache_set_capacity_missing_value_is_structured_fault) {
    load_dll();
    void* inst = g_syms.create(&g_host, "buffer");
    auto j = xi::Json::parse(send_cmd(inst, R"({"command":"set_capacity"})"));
    XI_EXPECT(j["error"].as_string() == xi::contract::kMissingInput);
    XI_EXPECT(j["key"].as_string() == std::string(xi::cache::keys::kValue));
    g_syms.destroy(inst);
}

XI_TEST(cache_unknown_command_falls_through_to_get_def) {
    load_dll();
    void* inst = g_syms.create(&g_host, "buffer");
    // A driver "status" idiom: an unknown command returns the def, NOT an error.
    xi::cache::Status st{ send_cmd(inst, R"({"command":"__stat__"})") };
    XI_EXPECT(st.valid());
    XI_EXPECT(st.capacity() >= 1);
    g_syms.destroy(inst);
}

XI_TEST(cache_happy_path_capture_replay_status) {
    load_dll();
    void* inst = g_syms.create(&g_host, "buffer");

    // Config via the typed builder.
    std::string cfg = xi::cache::Config().capacity(4);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);
    XI_EXPECT(xi::cache::Status{ get_def(inst) }.capacity() == 4);

    // Capture two records; the buffered ack climbs (read through the extractor).
    XI_EXPECT(capture_one(inst).buffered() == 1);
    XI_EXPECT(capture_one(inst).buffered() == 2);
    XI_EXPECT(xi::cache::Status{ get_def(inst) }.count() == 2);

    // replay_last re-emits one buffered record through the host emit sink.
    g_emit_count = 0;
    send_cmd(inst, xi::cache::Command::replay_last(1));
    XI_EXPECT(g_emit_count == 1);

    // clear empties the ring.
    send_cmd(inst, xi::cache::Command::clear());
    XI_EXPECT(xi::cache::Status{ get_def(inst) }.count() == 0);

    g_syms.destroy(inst);
}

XI_TEST(cache_config_schema_skew_is_rejected) {
    load_dll();
    void* inst = g_syms.create(&g_host, "buffer");
    XI_EXPECT(g_syms.set_def(inst, xi::cache::Config().capacity(8).dump().c_str()) == 0);
    std::string skew = xi::Json::object()
        .set(xi::contract::kSchemaKey, 999)
        .set(xi::cache::keys::kCapacity, 3).dump();
    XI_EXPECT(g_syms.set_def(inst, skew.c_str()) != 0);
    XI_EXPECT(xi::cache::Status{ get_def(inst) }.capacity() == 8);   // unchanged
    g_syms.destroy(inst);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
