//
// expose_script_push_test.cpp — polaris2 gate P2 (docs/new_gen/10): the
// EXPOSE-FROM-SCRIPT proof. A script that holds a sealed Pack (t.pack(), or any
// ScriptPack) pushes it to expose WITHOUT an intervening plugin, and the pushed
// XEX1-v2 bytes are BYTE-IDENTICAL to a direct host-side dump of the same pack
// — identity is the whole design point (memory ≈ wire ≈ disk, doc 07).
//
// This test drives the REAL surfaces on both sides of the seam:
//
//   * SCRIPT SIDE — the genuine SDK path a compiled script runs: an
//     xi::ScriptPack built exactly the way xi::Trigger builds t.pack() (own
//     handle ref + keepalive), pushed via xi::use("sink0").push(pack) through
//     the g_use_push_pack_fn_ thunk (this TU plays the script DLL: it defines
//     the g_* globals xi_script_support.hpp would define).
//   * SINK SIDE — the shipped expose plugin loaded through the genuine C-ABI
//     path, its xi.pack@1 door driven under CAbiInstanceAdapter, output
//     captured off the host binary sink.
//   * HOST THUNK — this test's push callback mirrors the service's
//     use_push_pack_cb staging discipline (service_sinks.cpp): retain the
//     handle BEFORE returning (the script's ref may die right after push()),
//     stage, flush later in call order via run_pack_door, drop the ack, release
//     the staged ref. (The real service cb is not linkable outside the backend
//     binary — same test seam as every other staged-sink behavior.)
//
// LIFETIME PROOF: the script's ScriptPack ref AND the test's own seal ref are
// both dropped BEFORE the staged flush — the staged retain alone must keep the
// sealed pack (and its pool image) alive across the seam. Zero-copy: the same
// xi_pack_handle flows end to end; PackRegistry/ImagePool balance to zero.
//
// LoadLibraryA/GetProcAddress/HMODULE/GetLastError for the loader below:
// real <windows.h> on Windows, dlopen/dlsym shim on POSIX (same names).
#include <xi/xi_dynlib.hpp>

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_binary_sink.hpp>    // capture emit_binary (the plugin -> WS byte pipe)
#include <xi/xi_use.hpp>            // the SCRIPT SDK surface: ScriptPack + use().push()

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef EXPOSE_DLL_PATH
#define EXPOSE_DLL_PATH "xi-expose.dll"
#endif

// ---- the script DLL's callback globals (this TU plays the script role) ------
// xi_use.hpp declares these extern; a real script gets them from
// xi_script_support.hpp (force-included). Only the push thunk is wired here.
void* g_use_process_fn_    = nullptr;
void* g_use_exchange_fn_   = nullptr;
void* g_use_host_api_      = nullptr;
void* g_trigger_info_fn_   = nullptr;
void* g_trigger_image_fn_  = nullptr;
void* g_trigger_sources_fn_ = nullptr;
void* g_trigger_leader_fn_ = nullptr;
void* g_trigger_meta_fn_   = nullptr;
void* g_use_push_pack_fn_  = nullptr;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// --- capture the plugin's emit_binary off the host sink ---------------------
static std::vector<std::vector<uint8_t>> g_emitted;
static void capture_binary(const void* data, int len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    g_emitted.emplace_back(p, p + (len > 0 ? len : 0));
}

// --- the host push thunk: the service's use_push_pack_cb staging, mirrored ---
static xi::CAbiInstanceAdapter* g_sink = nullptr;   // the "InstanceRegistry" of this test
static std::vector<std::pair<std::string, xi_pack_handle>> g_push_staged;

static int test_use_push_pack_cb(const char* name, xi_pack_handle pack) {
    if (!name || pack == XI_PACK_NULL) return -1;
    if (!g_sink || std::string(name) != "sink0") return -1;   // no such instance
    if (!g_sink->has_pack_door()) return -4;
    // Sink target: retain our staged ref BEFORE returning (the script's own
    // ScriptPack ref may die right after push() returns), then stage.
    xi::PackRegistry::instance().retain(pack);
    g_push_staged.emplace_back(name, pack);
    return 0;
}

// Frame-ordered flush (the service does this after the inspect, inside the
// EmitTurn gate): deliver each staged pack AS-IS to the door, drop the ack,
// release the staged ref.
static void flush_push_staged() {
    for (auto& [name, h] : g_push_staged) {
        xi_pack_handle ack = g_sink->run_pack_door(h);
        if (ack != XI_PACK_NULL) xi::PackRegistry::instance().release(ack);
        xi::PackRegistry::instance().release(h);
    }
    g_push_staged.clear();
}

int main() {
    std::printf("[test] expose-from-script (use(\"expose\").push(pack) -> XEX1-v2 identity)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_pack_v1* fi = xi::pack_v1_iface();
    xi::binary_sink() = &capture_binary;

    const int base_live = xi::ImagePool::instance().cumulative().live_now;

    HMODULE dll = LoadLibraryA(EXPOSE_DLL_PATH);
    if (!dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", EXPOSE_DLL_PATH, GetLastError()); return 1; }
    auto factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(factory != nullptr);
    if (!factory) return 1;
    void* inst = factory(&host, "sink0");
    CHECK(inst != nullptr);
    if (!inst) return 1;
    auto expose = std::make_unique<xi::CAbiInstanceAdapter>("sink0", "sink0", dll, inst);
    CHECK(expose->has_pack_door());
    g_sink = expose.get();

    // Canonical wire: v2 opt-in + a watched channel (unsubscribed = no push).
    CHECK(expose->set_def("{\"frame_wire_v2\":true}"));
    expose->exchange("{\"command\":\"subscribe\",\"channels\":[\"cam0\"]}");

    // One sealed pack, HOST-BUILT (per the gate note: don't depend on the
    // sibling script-built-pack API): reserved $channel/$seq + a generic mix.
    std::vector<uint8_t> pixels = {10, 20, 30, 40};   // 2x2x1 gray
    xi_pack_builder b = fi->builder_new();
    fi->builder_add_str(b, "$channel", "cam0", 4);
    fi->builder_add_i64(b, "$seq", 7);
    fi->builder_add_i64(b, "count", 42);
    fi->builder_add_f64(b, "score", 1.5);
    fi->builder_add_str(b, "label", "ok", 2);
    fi->builder_add_image(b, "frame", 2, 2, 1, pixels.data());
    xi_pack_handle pk = fi->builder_seal(b);
    CHECK(pk != XI_PACK_NULL);

    // ---------------------------------------------------------------------
    // (A) Direct host-side dump — the reference bytes.
    // ---------------------------------------------------------------------
    SECTION("reference: direct host-side pack-door dump");
    g_emitted.clear();
    {
        xi_pack_handle ack = expose->run_pack_door(pk);
        CHECK(ack != XI_PACK_NULL);
        if (ack) fi->release(ack);
        CHECK(g_emitted.size() == 1);
    }
    std::vector<uint8_t> direct_bytes = g_emitted.empty() ? std::vector<uint8_t>{} : g_emitted[0];
    CHECK(direct_bytes.size() > 5 && direct_bytes[0] == 'X' && direct_bytes[3] == '1');

    // ---------------------------------------------------------------------
    // (B) Older host / degraded: push() with no callback wired returns false.
    // ---------------------------------------------------------------------
    SECTION("degrade: no host thunk wired -> push() == false, nothing staged");
    {
        fi->retain(pk);
        xi::ScriptPack sp(pk, fi, std::shared_ptr<const void>(
            nullptr, [fi, pk](const void*) { fi->release(pk); }));
        CHECK(!xi::use("sink0").push(sp));            // g_use_push_pack_fn_ null
        CHECK(g_push_staged.empty());
    }

    // ---------------------------------------------------------------------
    // (C) The script push: same handle, through the real SDK surface.
    // ---------------------------------------------------------------------
    SECTION("script push: use(\"sink0\").push(t.pack()-style ScriptPack)");
    g_use_push_pack_fn_ = (void*)&test_use_push_pack_cb;   // host wires the thunk
    g_emitted.clear();
    {
        // Build the ScriptPack exactly as xi::Trigger builds t.pack(): its OWN
        // handle ref + a keepalive that releases it when the last copy dies.
        fi->retain(pk);
        xi::ScriptPack sp(pk, fi, std::shared_ptr<const void>(
            nullptr, [fi, pk](const void*) { fi->release(pk); }));
        CHECK(sp.valid());

        CHECK(xi::use("sink0").push(sp));             // staged, fire-and-forget
        CHECK(g_push_staged.size() == 1);
        CHECK(g_emitted.empty());                     // ordered sink: nothing until flush
    }                                                 // ScriptPack ref DIES here
    fi->release(pk);                                  // and OUR seal ref too:
    pk = XI_PACK_NULL;                                // only the staged ref remains
    flush_push_staged();                              // frame-ordered delivery

    CHECK(g_emitted.size() == 1);

    // ---------------------------------------------------------------------
    // (D) THE GATE: pushed bytes == direct host-side dump bytes, byte-for-byte.
    // ---------------------------------------------------------------------
    SECTION("identity: script-pushed XEX1-v2 == direct host dump (byte-for-byte)");
    CHECK(!g_emitted.empty() && g_emitted[0] == direct_bytes);

    // The sink really processed two frames on the same channel (ack path live).
    {
        std::string s = expose->exchange("{\"command\":\"list_channels\"}");
        CHECK(s.find("\"seen\":2") != std::string::npos);
    }

    // ---------------------------------------------------------------------
    // (E) Guard rails: empty pack + unknown instance are honest failures.
    // ---------------------------------------------------------------------
    SECTION("guards: empty pack / unknown instance -> push() == false");
    CHECK(!xi::use("sink0").push(xi::ScriptPack{}));      // empty: no thunk call
    {
        xi_pack_builder b2 = fi->builder_new();
        fi->builder_add_i64(b2, "x", 1);
        xi_pack_handle pk2 = fi->builder_seal(b2);
        fi->retain(pk2);
        xi::ScriptPack sp2(pk2, fi, std::shared_ptr<const void>(
            nullptr, [fi, pk2](const void*) { fi->release(pk2); }));
        CHECK(!xi::use("no_such_sink").push(sp2));        // -1: honest miss
        fi->release(pk2);
    }
    CHECK(g_push_staged.empty());

    // ---- teardown + pool/registry balance (the zero-copy leak oracle) ----
    xi::binary_sink() = nullptr;
    g_sink = nullptr;
    expose.reset();          // ~CAbiInstanceAdapter -> xi_plugin_destroy + leak sweep
    FreeLibrary(dll);

    CHECK(xi::ImagePool::instance().cumulative().live_now == base_live);
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
