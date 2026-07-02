//
// test_json_source.cpp — developer-side tests for the json_source plugin's data
// contract (docs/new_gen/02-plugin-data-contract.md).
//
// json_source EMITS user-authored JSON, so its output is an OPEN schema (there
// is no typed Output extractor). Its CONTROL surface — commands, config, and the
// process-input patch shape — IS a fixed vocabulary, and that is what the typed
// view (json_source_io.h) and these tests cover:
//
//   1. a required command payload ("value" for set_data) missing → a STRUCTURED
//      fault reply, not a silent no-op;
//   2. a CONFIG schema skew → set_def rejects it (returns non-zero) and leaves
//      the stored JSON unchanged;
//   3. the HAPPY PATH: Config / Command builders drive the stored JSON, and a
//      process() patch built with xi::json_source::Patch mutates the emitted
//      record (read back with a raw key — the output being open is the point).
//
// Build produces json_source_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_test.hpp>
#include <xi/xi_record.hpp>
#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>

#include "json_source_io.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef JSON_SOURCE_DLL_PATH
#define JSON_SOURCE_DLL_PATH "xi-json_source.dll"
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

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(JSON_SOURCE_DLL_PATH);
    if (!g_dll) { std::fprintf(stderr, "failed to load %s (err %lu)\n", JSON_SOURCE_DLL_PATH, GetLastError()); std::exit(2); }
    g_syms.create   = (xi_plugin_create_fn)  GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy  = (xi_plugin_destroy_fn) GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.process  = (xi_plugin_process_fn) GetProcAddress(g_dll, "xi_plugin_process");
    g_syms.exchange = (xi_plugin_exchange_fn)GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def  = (xi_plugin_get_def_fn) GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def  = (xi_plugin_set_def_fn) GetProcAddress(g_dll, "xi_plugin_set_def");
    if (!g_syms.create || !g_syms.destroy || !g_syms.process || !g_syms.exchange || !g_syms.get_def || !g_syms.set_def) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n"); std::exit(2);
    }
}

static std::string send_cmd(void* inst, const std::string& cmd) {
    char buf[1024];
    int n = g_syms.exchange(inst, cmd.c_str(), buf, sizeof(buf));
    return (n > 0) ? std::string(buf, n) : std::string();
}
static std::string get_def(void* inst) {
    char buf[1024];
    int n = g_syms.get_def(inst, buf, sizeof(buf));
    return (n > 0) ? std::string(buf, n) : std::string();
}

// Run process() once with a JSON input record (no images). Returns the output
// Record reconstructed from the returned JSON.
static xi::Record run_process(void* inst, const std::string& in_json) {
    xi_record in{};
    in.images      = nullptr;
    in.image_count = 0;
    in.data        = reinterpret_cast<const uint8_t*>(in_json.c_str());
    in.len         = static_cast<int32_t>(in_json.size());

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
    return xi::Record::from_json_bytes(reinterpret_cast<const uint8_t*>(js.data()), js.size());
}

// --- Tests -----------------------------------------------------------------

XI_TEST(json_source_set_data_missing_value_is_structured_fault) {
    load_dll();
    void* inst = g_syms.create(nullptr, "src");
    // set_data with no "value" — must fail loud, not silently keep the old JSON.
    std::string reply = send_cmd(inst, R"({"command":"set_data"})");
    auto j = xi::Json::parse(reply);
    XI_EXPECT(j["error"].as_string() == xi::contract::kMissingInput);
    XI_EXPECT(j["key"].as_string() == std::string(xi::json_source::keys::kValue));
    g_syms.destroy(inst);
}

XI_TEST(json_source_config_schema_skew_is_rejected) {
    load_dll();
    void* inst = g_syms.create(nullptr, "src");
    // Seed a known payload through the happy config path.
    std::string good = xi::json_source::Config().data(R"({"tag":"keep"})");
    XI_EXPECT(g_syms.set_def(inst, good.c_str()) == 0);

    // A config built against a future schema is rejected; stored data unchanged.
    std::string skew = std::string("{\"") + xi::contract::kSchemaKey +
                       "\":999,\"" + xi::json_source::keys::kData + "\":{\"tag\":\"nope\"}}";
    XI_EXPECT(g_syms.set_def(inst, skew.c_str()) != 0);   // rejected
    auto def = xi::Json::parse(get_def(inst));
    XI_EXPECT(def[xi::json_source::keys::kData]["tag"].as_string() == "keep");
    g_syms.destroy(inst);
}

XI_TEST(json_source_happy_path_config_command_patch) {
    load_dll();
    void* inst = g_syms.create(nullptr, "src");

    // Config builder sets the stored JSON.
    std::string cfg = xi::json_source::Config().data(R"({"n":1})");
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::json_source::keys::kData]["n"].as_int() == 1);

    // Command builder replaces it.
    send_cmd(inst, xi::json_source::Command::set_data(R"({"n":5})"));
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::json_source::keys::kData]["n"].as_int() == 5);

    // A per-emit patch (open value) mutates ONLY the emitted record; the output
    // is open schema, so it's read back with a raw key.
    xi::Record out = run_process(inst, xi::json_source::Patch::single(".n", "7"));
    XI_EXPECT(out.get_int("n") == 7);
    // Stored JSON is untouched by the patch.
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::json_source::keys::kData]["n"].as_int() == 5);

    g_syms.destroy(inst);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
