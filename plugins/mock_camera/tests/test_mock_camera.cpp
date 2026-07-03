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
#include <xi/xi_pack_abi.hpp>   // control-door test: host pack plane + vtable
#include <xi/xi_test.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_contract.hpp>

#include "mock_camera_io.gen.h"

#ifdef _WIN32
  #include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

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

// --- Gain knob (ex-feedback) -------------------------------------------------
//
// The pack-mode brightness multiplier: settable via config (set_def) and the
// set_gain command (exchange), always clamped to [0.05, 8.0], surfaced in
// get_def. The RECORD emit path never scales — the knob only exists so a
// closed-loop script can steer the PACK plant (examples/qa_pack_feedback).

XI_TEST(mock_camera_gain_knob_config_command_and_clamp) {
    load_dll();
    void* inst = g_syms.create(&g_host, "cam");
    namespace mck = xi::mock_camera::keys;

    // Default is the identity.
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 1.0);

    // Config route (typed builder) + clamp on the low side.
    std::string cfg = xi::mock_camera::Config().gain(0.5);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 0.5);
    cfg = xi::mock_camera::Config().gain(0.0001);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 0.05);

    // Command route (typed builder) + clamp on the high side.
    send_cmd(inst, xi::mock_camera::Command::set_gain(2.0));
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 2.0);
    send_cmd(inst, xi::mock_camera::Command::set_gain(99.0));
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 8.0);

    // Same fail-loud contract as set_fps: a value-less set_gain is a fault.
    auto j = xi::Json::parse(send_cmd(inst, R"({"command":"set_gain"})"));
    XI_EXPECT(j["error"].as_string() == xi::contract::kMissingInput);
    XI_EXPECT(j["key"].as_string() == std::string(mck::kValue));

    g_syms.destroy(inst);
}

// --- Control door (ex-feedback) ----------------------------------------------
//
// mock_camera's own xi.pack@1 door — the actuation seam of the closed loop. A
// control pack {command:"set_gain", value} acks with the clamped gain echoed
// (the sealed door output IS the reply); a command-less pack faults loud.

XI_TEST(mock_camera_control_door_set_gain_and_fail_loud) {
    load_dll();
    namespace mck = xi::mock_camera::keys;
    xi::install_pack_abi();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    XI_EXPECT(fi != nullptr);
    auto gi = reinterpret_cast<const void* (*)(const char*, uint32_t)>(
        GetProcAddress(g_dll, "xi_plugin_get_interface"));
    XI_EXPECT(gi != nullptr);                 // the door export exists...
    const xi_pack_proc_v1* door =
        gi ? static_cast<const xi_pack_proc_v1*>(gi("xi.pack", 1)) : nullptr;
    XI_EXPECT(door && door->process);         // ...and answers xi.pack@1
    if (!fi || !door || !door->process) return;

    void* inst = g_syms.create(&g_host, "cam");

    // set_gain round-trip: ack echoes command + clamped value; def mirrors it.
    xi_pack_builder b = fi->builder_new();
    fi->builder_add_str(b, mck::kCommand, mck::kSetGain,
                        (int32_t)std::strlen(mck::kSetGain));
    fi->builder_add_f64(b, mck::kValue, 0.25);
    xi_pack_handle in  = fi->builder_seal(b);
    xi_pack_handle out = door->process(inst, in);
    XI_EXPECT(out != XI_PACK_NULL);
    if (out != XI_PACK_NULL) {
        const char* s = nullptr; int32_t n = 0; double g = -1;
        XI_EXPECT(fi->get_str(out, "$fault", &s, &n) == 0);        // no fault
        XI_EXPECT(fi->get_f64(out, mck::kGain, &g) == 1 && g == 0.25);
        XI_EXPECT(fi->get_str(out, mck::kAck, &s, &n) == 1 &&
                  std::string(s, (size_t)n) == mck::kSetGain);
        fi->release(out);
    }
    fi->release(in);
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 0.25);

    // Fail-loud: no command selector -> a sealed $fault pack, never a no-op.
    b = fi->builder_new();
    fi->builder_add_f64(b, mck::kValue, 1.0);
    in  = fi->builder_seal(b);
    out = door->process(inst, in);
    XI_EXPECT(out != XI_PACK_NULL);
    if (out != XI_PACK_NULL) {
        const char* s = nullptr; int32_t n = 0;
        XI_EXPECT(fi->get_str(out, "$fault", &s, &n) == 1 &&
                  std::string(s, (size_t)n) == xi::contract::kMissingInput);
        fi->release(out);
    }
    fi->release(in);
    // The knob is untouched by the faulted control pack.
    XI_EXPECT(xi::Json::parse(get_def(inst))[mck::kGain].as_double(-1) == 0.25);

    g_syms.destroy(inst);
}

// --- Worker-path test ------------------------------------------------------
//
// mock_camera's start/stop drives a background source worker (xi::spawn_worker)
// that paints a pool_image() and hands it over via emit(). This test captures
// the emitted records (by installing an emit_record on the host table) and
// checks the port's three promises: the worker actually emits, each record is
// the frame contract shape, and a clean start/stop/destroy cycle balances every
// pooled frame's refcount (no leak — the hazard the raw-emit path risked).

struct EmitCapture {
    std::atomic<int>  frames{0};
    std::atomic<int>  last_count{0};
    std::atomic<bool> wrong_shape{false};
    std::atomic<bool> key_ok{true};
    std::atomic<int>  w{0}, h{0}, c{0};
    void reset() {
        frames = 0; last_count = 0; wrong_shape = false; key_ok = true;
        w = 0; h = 0; c = 0;
    }
};
static EmitCapture g_cap;

// Runs on the plugin's worker thread — keep it lock-free (atomics + the pool's
// own lock-free getters).
static void capture_emit(const char* /*emitter*/, xi_trigger_id /*id*/,
                         const xi_record* rec, int64_t /*ts*/) {
    if (!rec) return;
    g_cap.frames.fetch_add(1);
    g_cap.last_count.store(rec->image_count);
    if (rec->image_count != 1) g_cap.wrong_shape.store(true);
    for (int i = 0; i < rec->image_count; ++i) {
        const xi_record_image& e = rec->images[i];
        if (!e.key || std::string(e.key) != std::string(xi::mock_camera::keys::kFrame))
            g_cap.key_ok.store(false);
        g_cap.w.store(g_host.image_width(e.handle));
        g_cap.h.store(g_host.image_height(e.handle));
        g_cap.c.store(g_host.image_channels(e.handle));
    }
}

XI_TEST(mock_camera_worker_emits_frames_and_leaves_no_leak) {
    load_dll();
    auto& pool = xi::ImagePool::instance();
    const int live_before = pool.cumulative().live_now;

    g_cap.reset();
    g_host.emit_record = &capture_emit;   // field the plugin's emit() forwards to

    void* inst = g_syms.create(&g_host, "cam");
    // Small frame + high fps so several frames land in a short window.
    std::string cfg = xi::mock_camera::Config().width(64).height(48).fps(60);
    XI_EXPECT(g_syms.set_def(inst, cfg.c_str()) == 0);

    send_cmd(inst, R"({"command":"start"})");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    send_cmd(inst, R"({"command":"stop"})");   // joins the worker

    XI_EXPECT(g_cap.frames.load() > 0);        // the worker actually emitted
    XI_EXPECT(!g_cap.wrong_shape.load());      // exactly one image per record
    XI_EXPECT(g_cap.last_count.load() == 1);
    XI_EXPECT(g_cap.key_ok.load());            // under the kFrame key
    XI_EXPECT(g_cap.w.load() == 64);
    XI_EXPECT(g_cap.h.load() == 48);
    XI_EXPECT(g_cap.c.load() == 3);            // RGB

    g_syms.destroy(inst);
    g_host.emit_record = nullptr;

    // Clean teardown: every pooled frame emitted through pool_image + emit() is
    // released, so live occupancy returns to where it started — a leak here would
    // be exactly the pooled-image leak the raw-thread/raw-emit path risked.
    XI_EXPECT(pool.cumulative().live_now == live_before);
}

int main() {
    auto results = xi::test::run_all();
    for (auto& r : results) if (!r.passed) return 1;
    return 0;
}
