//
// test_synced_stereo.cpp — developer-side tests for the synced_stereo plugin's
// data contract (docs/new_gen/02-plugin-data-contract.md).
//
// synced_stereo is a GATHERING SOURCE, so its script/UI-facing contract is its
// CONFIG (set_def), its COMMANDS (exchange), and the two-image FRAME it emits.
// These tests drive the built DLL's C ABI and check:
//
//   1. the CONFIG/COMMAND happy path built with xi::synced_stereo::Config /
//      Command takes effect (fps set + clamped) through get_def;
//   2. the emitted stereo FRAME — captured through a real host emit_record sink
//      driven by the "fire" command — is read END-TO-END through the typed
//      xi::synced_stereo::Frame extractor (both images present, right size).
//      This is the leaky-veneer guard: the extractor is proven to read exactly
//      what synced_stereo.cpp actually emits, not a hand-authored fixture.
//
// NOTE: synced_stereo.cpp is intentionally not compiled against the keys header
// yet (its worker/emit path is being ported to spawn_worker on a parallel
// branch). This test therefore also PINS the wire keys ("left"/"right"/"fps"/
// "value"/...): if the plugin's literals drift from synced_stereo_keys.h, the
// Frame extractor stops finding the images here and the test goes red.
//
// Build produces synced_stereo_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_test.hpp>
#include <xi/xi_json.hpp>

#include "synced_stereo_io.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>

#ifndef SYNCED_STEREO_DLL_PATH
#define SYNCED_STEREO_DLL_PATH "xi-synced_stereo.dll"
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

// Capture sink: the record the plugin emits under "fire", rebuilt as an
// xi::Record (images copied out of the pool handles) so the typed Frame
// extractor can read it exactly as a script would.
static xi::Record g_last_emit;
static int        g_emit_count = 0;

static void capture_emit(const char* /*emitter*/, xi_trigger_id /*id*/,
                         const xi_record* rec, int64_t /*ts*/) {
    xi::Record r;
    for (int i = 0; i < rec->image_count; ++i) {
        xi_image_handle h = rec->images[i].handle;
        if (!h) continue;
        r.image(rec->images[i].key,
                xi::Image(g_host.image_width(h), g_host.image_height(h),
                          g_host.image_channels(h), g_host.image_data(h)));
    }
    g_last_emit = std::move(r);
    ++g_emit_count;
}

static void load_dll() {
    if (g_dll) return;
    g_dll = LoadLibraryA(SYNCED_STEREO_DLL_PATH);
    if (!g_dll) { std::fprintf(stderr, "failed to load %s (err %lu)\n", SYNCED_STEREO_DLL_PATH, GetLastError()); std::exit(2); }
    g_syms.create   = (xi_plugin_create_fn)  GetProcAddress(g_dll, "xi_plugin_create");
    g_syms.destroy  = (xi_plugin_destroy_fn) GetProcAddress(g_dll, "xi_plugin_destroy");
    g_syms.exchange = (xi_plugin_exchange_fn)GetProcAddress(g_dll, "xi_plugin_exchange");
    g_syms.get_def  = (xi_plugin_get_def_fn) GetProcAddress(g_dll, "xi_plugin_get_def");
    g_syms.set_def  = (xi_plugin_set_def_fn) GetProcAddress(g_dll, "xi_plugin_set_def");
    if (!g_syms.create || !g_syms.destroy || !g_syms.exchange || !g_syms.get_def || !g_syms.set_def) {
        std::fprintf(stderr, "DLL missing required C ABI exports\n"); std::exit(2);
    }
    // Wire the emit sink so xi::emit_record (fired by the "fire" command) lands
    // in capture_emit. make_host_api() leaves this null.
    g_host.emit_record = &capture_emit;
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

XI_TEST(synced_stereo_config_and_command_take_effect) {
    load_dll();
    void* inst = g_syms.create(&g_host, "stereo");

    // Config via the typed builder (stamps the schema version).
    std::string cfg = xi::synced_stereo::Config().fps(30);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);   // 0 = accepted
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::synced_stereo::keys::kFps].as_int() == 30);

    // set_fps command via the typed builder.
    send_cmd(inst, xi::synced_stereo::Command::set_fps(25));
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::synced_stereo::keys::kFps].as_int() == 25);

    // fps clamps to [1, 120].
    send_cmd(inst, xi::synced_stereo::Command::set_fps(1000));
    XI_EXPECT(xi::Json::parse(get_def(inst))[xi::synced_stereo::keys::kFps].as_int() == 120);

    g_syms.destroy(inst);
}

XI_TEST(synced_stereo_fire_emits_both_frames_read_via_extractor) {
    load_dll();
    void* inst = g_syms.create(&g_host, "stereo");

    g_emit_count = 0;
    // Deterministic headless drive: emit exactly one correlated pair.
    send_cmd(inst, xi::synced_stereo::Command::fire(1));
    XI_EXPECT(g_emit_count == 1);

    // Read the emitted record through the typed extractor — both images present
    // in ONE record (that colocation is the sync guarantee) and correctly sized.
    xi::synced_stereo::Frame f{ g_last_emit };
    XI_EXPECT(f.has_both());
    XI_EXPECT(f.left().width == 320 && f.left().height == 240);
    XI_EXPECT(f.right().width == 320 && f.right().height == 240);

    g_syms.destroy(inst);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
