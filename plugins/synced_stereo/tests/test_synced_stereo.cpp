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
//      what synced_stereo.cpp actually emits, not a hand-authored fixture;
//   3. polaris2 wave-2 (docs/new_gen/10 Pack migration, gate P1) — with
//      pack_mode ON, one "fire" produces exactly ONE trigger carrying ONE sealed
//      xi.pack@1 Pack whose entries are exactly {left, right, seq}, both images
//      correlated by that seq (the GATHERING invariant, now in a single sealed
//      container), and pooled-handle balance holds across fire AND start/stop.
//
// The default-mode Record path PINS the wire keys ("left"/"right") because the
// plugin still emits those literally: if they drift, the Frame extractor stops
// finding the images and test (2) goes red. The pack path is keyed off the
// generated synced_stereo_keys.gen.h the plugin now compiles against.
//
// Build produces synced_stereo_test.exe. Run it; exit code 0 = all pass.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_pack_abi.hpp>     // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_trigger_bus.hpp>  // install_trigger_hook + the pack dispatch sink
#include <xi/xi_test.hpp>
#include <xi/xi_json.hpp>

#include "synced_stereo_io.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// --- Pack mode (polaris2 wave-2) -------------------------------------------
// The pack path routes through emit(PackOut&&) -> host xi.pack@1 emit_pack ->
// TriggerBus, NOT the host emit_record sink the OFF-path tests use. So this
// test stands up its OWN host with the pack plane + the trigger hook, and
// captures the emitted Pack off the bus sink. emit_pack fires the sink
// synchronously on the emitting thread, so the synchronous "fire" command needs
// no waiting; start/stop uses the async worker and does wait.

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// Enable pack mode on an instance (no typed setter — the hand-written _io.h
// Config only exposes fps; a real driver without one hand-builds this JSON).
static std::string pack_mode_cfg(bool on) {
    return xi::Json::object()
        .set(xi::contract::kSchemaKey, xi::synced_stereo::kSchemaVersion)
        .set(xi::synced_stereo::keys::kPackMode, on)
        .dump();
}

XI_TEST(synced_stereo_pack_mode_gathers_left_right_seq_under_one_trigger) {
    namespace k = xi::synced_stereo::keys;

    xi::install_pack_abi();                                  // publish xi.pack@1 (idempotent)
    xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(host);                          // route emit_* -> TriggerBus
    const xi_pack_v1* fi = xi::pack_v1_iface();

    load_dll();
    const int base = pool_live();
    {
        void* inst = g_syms.create(&host, "stereo_pack");

        // Capture emitted packs off the bus (sink fires synchronously here).
        std::mutex m;
        std::vector<xi::TriggerEvent> got;
        xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) {
            std::lock_guard<std::mutex> lk(m); got.push_back(std::move(ev));
        });

        // get_def reflects pack_mode; opting in takes effect.
        XI_EXPECT(g_syms.set_def(inst, pack_mode_cfg(true).c_str()) == 0);
        XI_EXPECT(xi::Json::parse(get_def(inst))[k::kPackMode].as_bool() == true);

        // ONE fire → exactly ONE trigger carrying ONE sealed Pack (the gathering
        // invariant: both images ride a single sealed container, not two events).
        send_cmd(inst, xi::synced_stereo::Command::fire(1));

        xi_pack_handle cap = XI_PACK_NULL;
        {
            std::lock_guard<std::mutex> lk(m);
            XI_EXPECT(got.size() == 1);
            if (!got.empty()) {
                cap = got[0].pack;
                XI_EXPECT(got[0].images.empty());   // the pack IS the payload, no image bag
            }
        }
        XI_EXPECT(cap != XI_PACK_NULL);             // it rode the Pack plane, not a Record

        if (cap != XI_PACK_NULL) {
            // Exactly three entries, and exactly the set {left, right, seq}.
            XI_EXPECT(fi->count(cap) == 3);
            bool saw_left = false, saw_right = false, saw_seq = false;
            for (int i = 0; i < fi->count(cap); ++i) {
                int32_t len = 0; const char* kp = fi->key_at(cap, i, &len);
                std::string key(kp ? kp : "", (size_t)(kp ? len : 0));
                if (key == k::kLeft)  saw_left  = true;
                else if (key == k::kRight) saw_right = true;
                else if (key == k::kSeq)   saw_seq   = true;
                else XI_EXPECT(false);              // no stray entries
            }
            XI_EXPECT(saw_left && saw_right && saw_seq);

            // Both images present, correctly sized, single channel.
            xi_pack_image L{}, R{};
            XI_EXPECT(fi->get_image(cap, k::kLeft,  &L) == 1);
            XI_EXPECT(fi->get_image(cap, k::kRight, &R) == 1);
            XI_EXPECT(L.width == 320 && L.height == 240 && L.channels == 1);
            XI_EXPECT(R.width == 320 && R.height == 240 && R.channels == 1);

            // The seq entry and the seq stamped into BOTH images agree — proof the
            // two frames and the counter all came from the SAME event.
            int64_t seq = -1;
            XI_EXPECT(fi->get_i64(cap, k::kSeq, &seq) == 1);
            XI_EXPECT(seq == 0);                    // first fire on a fresh instance
            int32_t ls = -1, rs = -1;
            std::memcpy(&ls, L.pixels, sizeof(ls));
            std::memcpy(&rs, R.pixels, sizeof(rs));
            XI_EXPECT(ls == (int32_t)seq && rs == (int32_t)seq);
        }

        // Release the captured event's pack ref (the dispatcher's job).
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& ev : got)
                if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
            got.clear();
        }

        // Pool balance across the async worker path too: start, let it emit,
        // stop, drain every captured pack.
        send_cmd(inst, xi::synced_stereo::Command::start());
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        send_cmd(inst, xi::synced_stereo::Command::stop());
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& ev : got)
                if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
            got.clear();
        }

        xi::TriggerBus::instance().clear_sink();
        g_syms.destroy(inst);
    }

    // Every pooled image the packs adopted is balanced, and no pack leaked.
    XI_EXPECT(pool_live() == base);
    XI_EXPECT(xi::PackRegistry::instance().live_frames() == 0);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
