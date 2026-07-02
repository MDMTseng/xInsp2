//
// test_config_swap_probe.cpp — developer-side tests for the config_swap_probe
// plugin's data contract (docs/new_gen/02-plugin-data-contract.md).
//
// config_swap_probe's process() is a no-op (no input contract), so it has no
// Input/Output pair. Its script/UI-facing surface is its CONFIG ({value}), its
// get_status COMMAND, and the double-slot STATUS that command returns. These
// tests drive the built DLL's C ABI and check:
//
//   1. the CONFIG + get_status happy path through xi::config_swap_probe::Config /
//      Command / Status — including the prepare→commit double-slot, whose whole
//      point is observable ONLY through the Status extractor;
//   2. a CONFIG schema skew → set_def rejects it (returns non-zero) and leaves
//      the live value unchanged. (There are no required process inputs to fail
//      loud on; schema rejection is this plugin's structured-error surface.)
//
// Build produces config_swap_probe_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_test.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_contract.hpp>

#include "config_swap_probe_io.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <string>

#ifndef CONFIG_SWAP_PROBE_DLL_PATH
#define CONFIG_SWAP_PROBE_DLL_PATH "xi-config_swap_probe.dll"
#endif

struct Syms {
    xi_plugin_create_fn   create   = nullptr;
    xi_plugin_destroy_fn  destroy  = nullptr;
    xi_plugin_process_fn  process  = nullptr;
    xi_plugin_exchange_fn exchange = nullptr;
    xi_plugin_get_def_fn  get_def  = nullptr;
    xi_plugin_set_def_fn  set_def  = nullptr;
    xi_plugin_prepare_fn  prepare  = nullptr;
    xi_plugin_commit_fn   commit   = nullptr;
};
static HMODULE g_dll = nullptr;
static Syms g_syms;

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(CONFIG_SWAP_PROBE_DLL_PATH);
    if (!g_dll) { std::fprintf(stderr, "failed to load %s (err %lu)\n", CONFIG_SWAP_PROBE_DLL_PATH, GetLastError()); std::exit(2); }
    g_syms.create   = (xi_plugin_create_fn)  GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy  = (xi_plugin_destroy_fn) GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.process  = (xi_plugin_process_fn) GetProcAddress(g_dll, "xi_plugin_process");
    g_syms.exchange = (xi_plugin_exchange_fn)GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def  = (xi_plugin_get_def_fn) GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def  = (xi_plugin_set_def_fn) GetProcAddress(g_dll, "xi_plugin_set_def");
    g_syms.prepare  = (xi_plugin_prepare_fn) GetProcAddress(g_dll, "xi_plugin_prepare");
    g_syms.commit   = (xi_plugin_commit_fn)  GetProcAddress(g_dll, "xi_plugin_commit");
    if (!g_syms.create || !g_syms.destroy || !g_syms.process || !g_syms.exchange ||
        !g_syms.get_def || !g_syms.set_def || !g_syms.prepare || !g_syms.commit) {
        std::fprintf(stderr, "DLL missing required C ABI exports (incl. staged prepare/commit)\n"); std::exit(2);
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
static void drive_process(void* inst) {
    std::string empty = "{}";
    xi_record in{};
    in.data = reinterpret_cast<const uint8_t*>(empty.c_str());
    in.len  = 2;
    xi_record_out out; xi_record_out_init(&out);
    g_syms.process(inst, &in, &out);
    xi_record_out_free(&out);
}
static xi::config_swap_probe::Status status(void* inst) {
    return xi::config_swap_probe::Status{ send_cmd(inst, xi::config_swap_probe::Command::get_status()) };
}

// --- Tests -----------------------------------------------------------------

XI_TEST(config_swap_probe_config_status_and_double_slot) {
    load_dll();
    void* inst = g_syms.create(nullptr, "probe");

    // Config via the typed builder (stamps the schema).
    std::string cfg = xi::config_swap_probe::Config().value(42);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::config_swap_probe::keys::kValue].as_int() == 42);

    // Status extractor reads the live slot; nothing staged yet.
    {
        auto s = status(inst);
        XI_EXPECT(s.active() == 42);
        XI_EXPECT(!s.has_staged());
        XI_EXPECT(s.staged_value() == -1);
    }

    // process() records what it observed — read it back through Status.
    drive_process(inst);
    drive_process(inst);
    {
        auto s = status(inst);
        XI_EXPECT(s.last_seen() == 42);
        XI_EXPECT(s.proc() == 2);
    }

    // Double-slot: prepare stages WITHOUT touching the live slot...
    std::string staged_cfg = xi::config_swap_probe::Config().value(99);
    XI_EXPECT(g_syms.prepare(inst, staged_cfg.c_str(), "") == 0);
    {
        auto s = status(inst);
        XI_EXPECT(s.active() == 42);        // live still old
        XI_EXPECT(s.has_staged());
        XI_EXPECT(s.staged_value() == 99);
    }

    // ...commit swaps staging -> live atomically.
    g_syms.commit(inst);
    {
        auto s = status(inst);
        XI_EXPECT(s.active() == 99);
        XI_EXPECT(!s.has_staged());
    }

    g_syms.destroy(inst);
}

XI_TEST(config_swap_probe_config_schema_skew_is_rejected) {
    load_dll();
    void* inst = g_syms.create(nullptr, "probe");

    XI_EXPECT(g_syms.set_def(inst, xi::config_swap_probe::Config().value(7).dump().c_str()) == 0);

    // A config built against a future schema is rejected; live value unchanged.
    std::string skew = xi::Json::object()
        .set(xi::contract::kSchemaKey, 999)
        .set(xi::config_swap_probe::keys::kValue, 123).dump();
    XI_EXPECT(g_syms.set_def(inst, skew.c_str()) != 0);
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::config_swap_probe::keys::kValue].as_int() == 7);

    g_syms.destroy(inst);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
