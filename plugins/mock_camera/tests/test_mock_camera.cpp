//
// test_mock_camera.cpp — developer-side tests for the mock_camera plugin's
// data contract (docs/new_gen/02-plugin-data-contract.md).
//
// mock_camera is a source, so its script/UI-facing contract is its CONFIG
// (set_def) and COMMANDS (exchange). These tests drive the built DLL's C ABI
// and check:
//
//   1. a required command payload ("value" for set_fps) missing → a STRUCTURED
//      fault reply, not a silent no-op;
//   2. a CONFIG schema skew → set_def rejects it (returns false) and leaves the
//      config unchanged;
//   3. the HAPPY PATH built with xi::mock_camera::Config / Command takes effect
//      (and fps clamps).
//
// Build produces mock_camera_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_contract.hpp>

#include "mock_camera_io.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <string>

#ifndef MOCK_CAMERA_DLL_PATH
#define MOCK_CAMERA_DLL_PATH "xi-mock_camera.dll"
#endif

struct Syms {
    xi_plugin_create_fn   create   = nullptr;
    xi_plugin_destroy_fn  destroy  = nullptr;
    xi_plugin_exchange_fn exchange = nullptr;
    xi_plugin_get_def_fn  get_def  = nullptr;
    xi_plugin_set_def_fn  set_def  = nullptr;
};
static HMODULE g_dll = nullptr;
static Syms g_syms;
static xi_host_api g_host = xi::ImagePool::make_host_api();

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(MOCK_CAMERA_DLL_PATH);
    if (!g_dll) { std::fprintf(stderr, "failed to load %s (err %lu)\n", MOCK_CAMERA_DLL_PATH, GetLastError()); std::exit(2); }
    g_syms.create   = (xi_plugin_create_fn)  GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy  = (xi_plugin_destroy_fn) GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.exchange = (xi_plugin_exchange_fn)GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def  = (xi_plugin_get_def_fn) GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def  = (xi_plugin_set_def_fn) GetProcAddress(g_dll, "xi_plugin_set_def");
    if (!g_syms.create || !g_syms.destroy || !g_syms.exchange || !g_syms.get_def || !g_syms.set_def) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n"); std::exit(2);
    }
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

// --- Tests -----------------------------------------------------------------

XI_TEST(mock_camera_set_fps_missing_value_is_structured_fault) {
    load_dll();
    void* inst = g_syms.create(&g_host, "cam");
    // set_fps with no "value" — must fail loud, not silently keep the old fps.
    std::string reply = send_cmd(inst, R"({"command":"set_fps"})");
    auto j = xi::Json::parse(reply);
    XI_EXPECT(j["error"].as_string() == xi::contract::kMissingInput);
    XI_EXPECT(j["key"].as_string() == std::string(xi::mock_camera::keys::kValue));
    g_syms.destroy(inst);
}

XI_TEST(mock_camera_config_schema_skew_is_rejected) {
    load_dll();
    void* inst = g_syms.create(&g_host, "cam");
    // Baseline fps.
    int fps0 = xi::Json::parse(get_def(inst))[xi::mock_camera::keys::kFps].as_int();
    // Config built against a future schema is rejected; config unchanged.
    std::string cfg = xi::Json::object()
        .set(xi::contract::kSchemaKey, 999)
        .set(xi::mock_camera::keys::kFps, 42).dump();
    int rc = g_syms.set_def(inst, cfg.c_str());
    XI_EXPECT(rc != 0);   // C ABI returns -1 when the plugin rejects the config
    int fps1 = xi::Json::parse(get_def(inst))[xi::mock_camera::keys::kFps].as_int();
    XI_EXPECT(fps1 == fps0);   // unchanged
    g_syms.destroy(inst);
}

XI_TEST(mock_camera_happy_path_via_config_and_command) {
    load_dll();
    void* inst = g_syms.create(&g_host, "cam");

    // Config via the typed builder.
    std::string cfg = xi::mock_camera::Config().width(320).height(240).fps(20);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);   // 0 = accepted
    auto def = xi::Json::parse(get_def(inst));
    XI_EXPECT(def[xi::mock_camera::keys::kWidth].as_int() == 320);
    XI_EXPECT(def[xi::mock_camera::keys::kFps].as_int() == 20);

    // Command via the typed builder.
    send_cmd(inst, xi::mock_camera::Command::set_fps(25));
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::mock_camera::keys::kFps].as_int() == 25);

    // fps clamps to [1, 60].
    send_cmd(inst, xi::mock_camera::Command::set_fps(1000));
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::mock_camera::keys::kFps].as_int() == 60);

    g_syms.destroy(inst);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
