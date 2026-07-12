//
// pack_pilot_test.cpp — the polaris2 wave-2 pilot pair, end to end against the
// REAL built DLLs (docs/new_gen/08 Wave 2, step 3-4).
//
// Loads mock_camera + blob_analysis through the genuine C-ABI load path and
// exercises the xi.pack@1 door both directions:
//
//   * PLUGIN-door probe — blob_analysis publishes xi_plugin_get_interface and
//     answers ("xi.pack", 1); mock_camera answers it too since ex-feedback
//     (its CONTROL door: the source emits packs AND accepts
//     control packs). The host adapter reflects both via has_pack_door().
//   * CHAINED FLOW — mock_camera emits a Pack through the bus;
//     the test (playing the gray-conversion step a script would own) converts
//     it to a "gray" pack and drives it through blob_analysis's pack door.
//   * DETERMINISTIC DOOR — a controlled white-square gray pack through the door
//     yields exactly one blob whose contour (nested msgpack) decodes back.
//   * FAIL-LOUD — a pack missing "gray" comes back as a normal sealed pack
//     stamped with the missing_input reason code (PackOut::fault).
//   * Pooled-handle balance across the whole pilot path.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>      // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_trigger_bus.hpp>    // the dispatch sink (TriggerBus)
#include <xi/xi_mp.hpp>             // decode blob_analysis's nested-contour msgpack

#include "blob_analysis_keys.gen.h"     // the pack schema keyset blob reads/writes
#include "mock_camera_keys.gen.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef BLOB_DLL_PATH
#define BLOB_DLL_PATH "xi-blob_analysis.dll"
#endif
#ifndef MOCK_DLL_PATH
#define MOCK_DLL_PATH "xi-mock_camera.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

namespace bkeys = xi::blob::keys;
namespace mkeys = xi::mock_camera::keys;

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// A loaded C-ABI plugin instance wrapped in the real host adapter.
struct LoadedPlugin {
    HMODULE dll = nullptr;
    void*   inst = nullptr;
    std::unique_ptr<xi::CAbiInstanceAdapter> adapter;
    xi_plugin_get_interface_fn get_iface = nullptr;
};

static LoadedPlugin load_plugin(const char* path, const char* name,
                                const xi_host_api* host) {
    LoadedPlugin lp;
    lp.dll = LoadLibraryA(path);
    if (!lp.dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", path, GetLastError()); ++g_failures; return lp; }
    auto factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(lp.dll, "xi_plugin_create"));
    CHECK(factory != nullptr);
    if (factory) lp.inst = factory(host, name);
    CHECK(lp.inst != nullptr);
    lp.get_iface = reinterpret_cast<xi_plugin_get_interface_fn>(GetProcAddress(lp.dll, "xi_plugin_get_interface"));
    if (lp.inst)
        lp.adapter = std::make_unique<xi::CAbiInstanceAdapter>(name, name, lp.dll, lp.inst);
    return lp;
}

// Decode blob_analysis's "blobs" nested-msgpack entry, returning (blob count,
// total contour points across all blobs). A recursive value-skip drives the
// pull Reader; we tap the "contour" arrays as we descend the first level of maps.
static bool decode_blobs(const uint8_t* mp, int32_t n, int& blob_count, int& total_contour) {
    blob_count = 0; total_contour = 0;
    xi::mp::Reader r(mp, (size_t)n);
    xi::mp::Element e;
    if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Array) return false;
    blob_count = (int)e.len;
    for (int bi = 0; bi < blob_count; ++bi) {
        xi::mp::Element m;
        if (r.next(m) != xi::mp::Status::Ok || m.kind != xi::mp::Kind::Map) return false;
        for (uint32_t pi = 0; pi < m.len; ++pi) {
            xi::mp::Element key;
            if (r.next(key) != xi::mp::Status::Ok || key.kind != xi::mp::Kind::Str) return false;
            std::string k(reinterpret_cast<const char*>(key.data), key.len);
            xi::mp::Element val;
            if (r.next(val) != xi::mp::Status::Ok) return false;
            if (val.kind == xi::mp::Kind::Array) {
                // "contour" is an array of {x,y} maps — count + skip its children.
                if (k == bkeys::kContour) total_contour += (int)val.len;
                for (uint32_t ci = 0; ci < val.len; ++ci) {
                    xi::mp::Element pt;
                    if (r.next(pt) != xi::mp::Status::Ok || pt.kind != xi::mp::Kind::Map) return false;
                    for (uint32_t pp = 0; pp < pt.len; ++pp) {
                        xi::mp::Element pk, pv;
                        if (r.next(pk) != xi::mp::Status::Ok) return false;
                        if (r.next(pv) != xi::mp::Status::Ok) return false;
                    }
                }
            }
            // scalars: already consumed by r.next(val).
        }
    }
    return true;
}

int main() {
    std::printf("[test] xi.pack@1 pilot pair (mock_camera -> blob_analysis pack door)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    // [v12 THE CUT: the Record trigger hook is gone — install_pack_abi() wires the emit_pack forwarder]
    const xi_pack_v1* fi = xi::pack_v1_iface();

    int base = pool_live();

    LoadedPlugin blob = load_plugin(BLOB_DLL_PATH, "det0", &host);
    LoadedPlugin mock = load_plugin(MOCK_DLL_PATH, "cam0", &host);
    if (!blob.inst || !mock.inst) { std::fprintf(stderr, "\nSETUP FAILED\n"); return 1; }

    // ---------------------------------------------------------------------
    // (A) Plugin-door probe: BOTH answer xi.pack@1 — blob's is the operator
    //     door; mock's is its CONTROL door (ex-feedback: the source
    //     emits packs AND accepts control packs on its own door).
    // ---------------------------------------------------------------------
    SECTION("plugin door probe: blob has xi.pack@1; mock (pack source) too");
    CHECK(blob.get_iface != nullptr);
    if (blob.get_iface) {
        CHECK(blob.get_iface("xi.pack", 1) != nullptr);
        CHECK(blob.get_iface("xi.pack", 2) == nullptr);   // only @1 published
        CHECK(blob.get_iface("xi.other", 1) == nullptr);
    }
    CHECK(blob.adapter->has_pack_door());
    CHECK(mock.get_iface != nullptr);        // ex-feedback: the control door
    if (mock.get_iface) {
        CHECK(mock.get_iface("xi.pack", 1) != nullptr);
        CHECK(mock.get_iface("xi.pack", 2) == nullptr);   // only @1 published
        CHECK(mock.get_iface("xi.other", 1) == nullptr);
    }
    CHECK(mock.adapter->has_pack_door());

    // ---------------------------------------------------------------------
    // (B) Deterministic door: a white square yields one blob whose contour
    //     round-trips through the nested msgpack.
    // ---------------------------------------------------------------------
    SECTION("pack door: white-square gray pack -> one blob with a traced contour");
    {
        const int W = 16, H = 16;
        std::vector<uint8_t> gray((size_t)W * H, 0);
        for (int y = 5; y < 10; ++y)
            for (int x = 5; x < 10; ++x) gray[y * W + x] = 255;   // a 5x5 white square

        xi_pack_builder b = fi->builder_new();
        fi->builder_add_image(b, bkeys::kGray, W, H, 1, gray.data());
        fi->builder_add_i64(b, bkeys::kThreshold, 128);
        fi->builder_add_i64(b, bkeys::kMinArea, 1);
        xi_pack_handle in = fi->builder_seal(b);

        xi_pack_handle out = blob.adapter->run_pack_door(in);
        CHECK(out != XI_PACK_NULL);
        if (out) {
            int64_t bc = -1, tu = -1;
            CHECK(fi->get_i64(out, bkeys::kBlobCount, &bc) == 1 && bc == 1);
            CHECK(fi->get_i64(out, bkeys::kThresholdUsed, &tu) == 1 && tu == 128);
            xi_pack_image bin{};
            CHECK(fi->get_image(out, bkeys::kBinary, &bin) == 1 && bin.width == W && bin.channels == 1);
            const void* mp = nullptr; int32_t ml = 0;
            CHECK(fi->get_mp(out, bkeys::kBlobs, &mp, &ml) == 1);
            int nblob = 0, ncontour = 0;
            CHECK(decode_blobs(static_cast<const uint8_t*>(mp), ml, nblob, ncontour));
            CHECK(nblob == 1);
            CHECK(ncontour > 0);      // the square's boundary was traced + emitted
            const char* fc = nullptr; int32_t fl = 0;
            CHECK(fi->get_str(out, "$fault", &fc, &fl) == 0);   // a real result, not a fault pack
            fi->release(out);
        }
        fi->release(in);
    }

    // ---------------------------------------------------------------------
    // (C) Fail-loud: a pack missing "gray" comes back stamped with the reason.
    // ---------------------------------------------------------------------
    SECTION("pack door: missing 'gray' -> a sealed $fault pack (missing_input)");
    {
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, bkeys::kThreshold, 100);   // no gray image
        xi_pack_handle in = fi->builder_seal(b);
        xi_pack_handle out = blob.adapter->run_pack_door(in);
        CHECK(out != XI_PACK_NULL);                      // a contract failure is still a pack
        if (out) {
            const char* code = nullptr; int32_t cl = 0;
            CHECK(fi->get_str(out, "$fault", &code, &cl) == 1);
            CHECK(cl > 0 && std::string(code, (size_t)cl) == "missing_input");
            int64_t bc;
            CHECK(fi->get_i64(out, bkeys::kBlobCount, &bc) == 0);   // no results on the failure pack
            fi->release(out);
        }
        fi->release(in);
    }

    // ---------------------------------------------------------------------
    // (D) Chained flow: mock_camera emits a Pack through the bus;
    //     convert to gray (the step a script owns) and drive blob's door.
    // ---------------------------------------------------------------------
    SECTION("chained: mock_camera pack-emit -> bus -> convert -> blob pack door");
    {
        std::mutex m;
        std::condition_variable cv;
        std::vector<xi::TriggerEvent> got;
        xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) {
            std::lock_guard<std::mutex> lk(m);
            got.push_back(std::move(ev));
            cv.notify_one();
        });

        // Configure a small, fast frame, then start the source (v12: the sealed
        // pack is the sole emit currency — no mode knob to flip).
        CHECK(mock.adapter->set_def(
            std::string("{\"") +
            mkeys::kWidth + "\":64,\"" + mkeys::kHeight + "\":48,\"" + mkeys::kFps + "\":30}"));
        mock.adapter->exchange(std::string("{\"") + mkeys::kCommand + "\":\"" + mkeys::kStart + "\"}");

        // Wait for at least one pack event (generous timeout — first pack is fast).
        xi_pack_handle cap = XI_PACK_NULL;
        {
            std::unique_lock<std::mutex> lk(m);
            bool ok = cv.wait_for(lk, std::chrono::seconds(5), [&] {
                for (auto& ev : got) if (ev.pack != XI_PACK_NULL) return true;
                return false;
            });
            CHECK(ok);
        }
        mock.adapter->exchange(std::string("{\"") + mkeys::kCommand + "\":\"" + mkeys::kStop + "\"}");

        // Take the first captured pack; verify mock's pack-emit contract.
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& ev : got) if (ev.pack != XI_PACK_NULL) { cap = ev.pack; break; }
        }
        CHECK(cap != XI_PACK_NULL);
        if (cap) {
            int64_t seq = -1;
            CHECK(fi->get_i64(cap, mkeys::kSeq, &seq) == 1 && seq >= 0);
            xi_pack_image rgb{};
            CHECK(fi->get_image(cap, mkeys::kFrame, &rgb) == 1 && rgb.channels == 3);

            // The gray-conversion step (a script's job) — build a "gray" pack.
            const int W = rgb.width, H = rgb.height;
            std::vector<uint8_t> gray((size_t)W * H);
            const uint8_t* px = static_cast<const uint8_t*>(rgb.pixels);
            for (int i = 0; i < W * H; ++i)
                gray[i] = (uint8_t)(((int)px[i*3] + px[i*3+1] + px[i*3+2]) / 3);

            xi_pack_builder b = fi->builder_new();
            fi->builder_add_image(b, bkeys::kGray, W, H, 1, gray.data());
            fi->builder_add_i64(b, bkeys::kThreshold, 200);   // pick the bright frame-counter strokes
            fi->builder_add_i64(b, bkeys::kMinArea, 1);
            xi_pack_handle gframe = fi->builder_seal(b);

            xi_pack_handle out = blob.adapter->run_pack_door(gframe);
            CHECK(out != XI_PACK_NULL);
            if (out) {
                // The chain ran end to end and produced a well-formed, non-fault pack.
                const char* code = nullptr; int32_t cl = 0;
                CHECK(fi->get_str(out, "$fault", &code, &cl) == 0);   // no fault
                int64_t bc = -1;
                CHECK(fi->get_i64(out, bkeys::kBlobCount, &bc) == 1 && bc >= 0);
                fi->release(out);
            }
            fi->release(gframe);
        }

        // Release every captured event's pack ref (the dispatcher's job).
        {
            std::lock_guard<std::mutex> lk(m);
            for (auto& ev : got)
                if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
            got.clear();
        }
        xi::TriggerBus::instance().clear_sink();
    }

    // ---------------------------------------------------------------------
    // Teardown + pooled-handle balance.
    // ---------------------------------------------------------------------
    blob.adapter.reset();   // ~CAbiInstanceAdapter -> xi_plugin_destroy + leak sweep
    mock.adapter.reset();
    FreeLibrary(blob.dll);
    FreeLibrary(mock.dll);

    CHECK(pool_live() == base);   // every pack + pooled image balanced
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
