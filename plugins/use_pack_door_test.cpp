//
// use_pack_door_test.cpp — polaris2 Gate P2 (docs/new_gen/10): script-side
// use()→pack-door CHAINING, end to end against the REAL built DLLs.
//
// The wave-2 pilot proved the door host-side (pack_pilot_test drives
// CAbiInstanceAdapter::run_pack_door directly). This test proves the SCRIPT
// surface over it: the genuine xi_use.hpp UseProxy::process(ScriptPack)
// overload — the same code a JIT-compiled inspection script runs — chained
// through a use_pack_process callback that mirrors the service's
// use_pack_process_cb (service_sinks.cpp), into blob_analysis's xi.pack@1 door.
// This TU plays the "script DLL" role: it OWNS the extern callback globals
// exactly as test_crash_leak_counter.cpp does.
//
// Covers:
//   1. DOOR ROUND-TRIP — a white-square gray pack through
//      xi::use("det0").process(pack) yields one blob (blob_count/threshold_used/
//      binary image all read back through the returned ScriptPack).
//   2. RESULT OWNERSHIP — the returned ScriptPack owns its handle: it stays
//      readable after the input pack AND every other ref are gone, and its
//      last copy releases the registry entry (live_frames balances).
//   3. TRIGGER CHAINING — the literal P2 story: an event pack on an explicit
//      xi_trigger_view → t.pack() → use("det0").process(t.pack()) → result.
//   4. FAIL-LOUD CONTRACT — input missing "gray" returns a NORMAL sealed pack
//      stamped "$fault"=missing_input (not an empty pack).
//   5. FAIL-CLOSED EDGES — unknown instance (-1), Record-only plugin with no
//      door (-4, mock_camera), empty input pack, and an older host (callback
//      null) all yield an empty bool-false ScriptPack, never a crash.
//   7. (U1, doc 15) FAULT ROUND-TRIP — ScriptPackBuilder::fault()/src() seal a
//      readable fault pack (is_fault/fault_reason/fault_key/fault_detail/src).
//   8. (U1) SHORT-CIRCUIT — a fault input with an otherwise-processable
//      payload never enters the door (call count unchanged), and the result
//      is a NEW fault pack: original reason + $seq carried, $src = the hop,
//      $prov appended.
//   9. (U1) PROVENANCE CHAIN across two chained doors — happy-path chain
//      det0→det1 accumulates $prov "det0/det1" (det1's plugin-minted contract
//      fault inherits det0's chain); a fault input short-circuited through
//      both hops accumulates the same chain without running either door.
//  10. (U1) NON-FAULT PACKS UNAFFECTED — the happy path still carries its
//      results, no $fault appears, and the door output is $src/$prov-stamped.
//  11. BALANCE (runs last) — PackRegistry live_frames + ImagePool live handles
//      return to baseline once every ScriptPack is dropped.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_instance.hpp>       // InstanceRegistry (the use() lookup table)
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_pack_contract.hpp>  // U1: is_fault/propagate_fault (the funnel short-circuit)
#include <xi/xi_script_pack.hpp>    // U1: ScriptPackBuilder (fault()/src() ergonomics)
#include <xi/xi_trigger_bus.hpp>    // install_trigger_hook
#include <xi/xi_use.hpp>            // the REAL script surface: use()/UseProxy/ScriptPack/Trigger

#include "blob_analysis_keys.gen.h"

#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
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

static int pool_live() { return xi::ImagePool::instance().cumulative().live_now; }

// xi_use.hpp declares these extern (the script DLL defines them static via
// xi_script_support.hpp). This host-role test OWNS the storage — the same
// idiom as test_crash_leak_counter.cpp / test_parallel_safety.cpp.
void* g_use_process_fn_      = nullptr;
void* g_use_exchange_fn_     = nullptr;
void* g_use_host_api_        = nullptr;
void* g_trigger_info_fn_     = nullptr;
void* g_trigger_image_fn_    = nullptr;
void* g_trigger_sources_fn_  = nullptr;
void* g_trigger_leader_fn_   = nullptr;
void* g_trigger_meta_fn_     = nullptr;
void* g_use_pack_process_fn_ = nullptr;   // Gate P2: the seam under test

// Mirror of service_sinks.cpp use_pack_process_cb, minus the item-14 fault
// machinery (no quarantine state in this harness): U1 fault short-circuit
// (the SAME xi::pack_contract::propagate_fault the service calls, doc 15) →
// registry lookup → adapter → door probe → run_pack_door. Same return-code
// contract the script maps. g_door_calls counts actual door entries so the
// short-circuit sections can PROVE the plugin never ran.
static int g_door_calls = 0;
static int use_pack_process(const char* name, xi_pack_handle in, xi_pack_handle* out) {
    if (out) *out = XI_PACK_NULL;
    if (!name || !out) return -1;
    if (xi::pack_contract::is_fault(xi::pack_v1_iface(), in)) {
        *out = xi::pack_contract::propagate_fault(xi::pack_v1_iface(), in, name);
        return 0;
    }
    auto inst = xi::InstanceRegistry::instance().find(name);
    if (!inst) return -1;
    auto* a = dynamic_cast<xi::CAbiInstanceAdapter*>(inst.get());
    if (!a || !a->has_pack_door()) return -4;
    try {
        ++g_door_calls;
        *out = a->run_pack_door(in);
        return 0;
    } catch (...) {
        return -2;
    }
}

// Wrap a freshly sealed handle (rc 1, ours) in an OWNING ScriptPack — the
// keepalive releases the emitter ref on the last copy, exactly as the t.pack()
// path ties its ref to Trigger::Data.
static xi::ScriptPack own_pack(xi_pack_handle h, const xi_pack_v1* fi) {
    return xi::ScriptPack(h, fi, std::shared_ptr<const void>(
        static_cast<const void*>(fi), [fi, h](const void*) { fi->release(h); }));
}

// Build + seal the blob-door happy-path input: a W×H gray image with a 5×5
// white square, threshold/min_area set for exactly one blob.
static xi_pack_handle build_square_pack(const xi_pack_v1* fi, int W = 16, int H = 16) {
    std::vector<uint8_t> gray((size_t)W * H, 0);
    for (int y = 5; y < 10; ++y)
        for (int x = 5; x < 10; ++x) gray[(size_t)y * W + x] = 255;
    xi_pack_builder b = fi->builder_new();
    fi->builder_add_image(b, bkeys::kGray, W, H, 1, gray.data());
    fi->builder_add_i64(b, bkeys::kThreshold, 128);
    fi->builder_add_i64(b, bkeys::kMinArea, 1);
    return fi->builder_seal(b);
}

int main() {
    std::printf("[test] Gate P2 — script use()->xi.pack@1 door chaining (UseProxy::process(ScriptPack))\n");

    xi::install_pack_abi();
    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(host);
    const xi_pack_v1* fi = xi::pack_v1_iface();

    // Load the pilot pair through the genuine C-ABI load path and register the
    // adapters where use()'s host callback looks them up.
    HMODULE blob_dll = LoadLibraryA(BLOB_DLL_PATH);
    HMODULE mock_dll = LoadLibraryA(MOCK_DLL_PATH);
    if (!blob_dll || !mock_dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary err %lu (blob=%p mock=%p)\n",
                     GetLastError(), (void*)blob_dll, (void*)mock_dll);
        return 1;
    }
    auto bfac = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(blob_dll, "xi_plugin_create"));
    auto mfac = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(mock_dll, "xi_plugin_create"));
    CHECK(bfac && mfac);
    if (!bfac || !mfac) return 1;
    void* binst = bfac(&host, "det0");
    void* binst1 = bfac(&host, "det1");   // U1: second door hop for the provenance chain
    void* minst = mfac(&host, "cam0");
    CHECK(binst && binst1 && minst);
    if (!binst || !binst1 || !minst) return 1;
    xi::InstanceRegistry::instance().add(
        std::make_shared<xi::CAbiInstanceAdapter>("det0", "blob_analysis", blob_dll, binst));
    xi::InstanceRegistry::instance().add(
        std::make_shared<xi::CAbiInstanceAdapter>("det1", "blob_analysis", blob_dll, binst1));
    xi::InstanceRegistry::instance().add(
        std::make_shared<xi::CAbiInstanceAdapter>("cam0", "mock_camera", mock_dll, minst));

    // Wire the script-side use() seam exactly as the host does per DLL load.
    g_use_host_api_        = (void*)&host;
    g_use_pack_process_fn_ = (void*)&use_pack_process;

    int    base_pool   = pool_live();
    size_t base_frames = xi::PackRegistry::instance().live_frames();

    // -----------------------------------------------------------------------
    SECTION("(1) door round-trip: use(\"det0\").process(pack) -> one blob");
    {
        auto in  = own_pack(build_square_pack(fi), fi);
        CHECK(in);
        auto out = xi::use("det0").process(in);
        CHECK(out.valid());
        if (out) {
            CHECK(out.get_i64(bkeys::kBlobCount).value_or(-1) == 1);
            CHECK(out.get_i64(bkeys::kThresholdUsed).value_or(-1) == 128);
            auto bin = out.get_image(bkeys::kBinary);
            CHECK(bin && bin->width == 16 && bin->height == 16 && bin->channels == 1);
            CHECK(!out.get_str("$fault"));                       // a real result, not a fault pack
            CHECK(out.tag_of(bkeys::kBlobCount) == XI_PACK_TAG_I64);
            CHECK(out.get_mp(bkeys::kBlobs).has_value());        // nested contour payload rode along
        }
    }   // both ScriptPacks drop here → registry balances (checked at the end)

    // -----------------------------------------------------------------------
    SECTION("(2) result ownership: output outlives the input pack + all other refs");
    {
        xi::ScriptPack out;
        {
            auto in = own_pack(build_square_pack(fi), fi);
            out = xi::use("det0").process(in);
        }   // input released; only `out`'s own ref keeps its pack alive
        CHECK(out.valid());
        CHECK(out.get_i64(bkeys::kBlobCount).value_or(-1) == 1); // still readable
        size_t live_with_out = xi::PackRegistry::instance().live_frames();
        CHECK(live_with_out == base_frames + 1);                 // exactly the result pack
        out = xi::ScriptPack{};                                  // drop the last ref
        CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    }

    // -----------------------------------------------------------------------
    SECTION("(3) trigger chaining: event pack -> t.pack() -> use(\"det0\").process");
    {
        xi_pack_handle ev = build_square_pack(fi);               // our emitter ref (rc 1)
        xi_trigger_view v{};
        v.is_active     = 1;
        v.id            = xi_trigger_id{0x1234, 0x5678};
        v.timestamp_us  = 1;
        v.dequeued_at_us = 2;
        v.leader_source = "cam0";
        v.pack          = ev;                                    // borrowed by the view
        v.host          = &host;
        xi::Trigger t(&v);                                       // retains its OWN pack ref
        fi->release(ev);                                         // drop the emitter ref
        CHECK(t.is_active());
        auto f = t.pack();
        CHECK(f.valid());                                        // the trigger's pack survived
        auto out = xi::use("det0").process(f);                   // THE P2 chain
        CHECK(out.valid());
        if (out) CHECK(out.get_i64(bkeys::kBlobCount).value_or(-1) == 1);
    }

    // -----------------------------------------------------------------------
    SECTION("(4) fail-loud contract: missing 'gray' -> sealed $fault pack, not empty");
    {
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_i64(b, bkeys::kThreshold, 100);          // no gray image
        auto in  = own_pack(fi->builder_seal(b), fi);
        auto out = xi::use("det0").process(in);
        CHECK(out.valid());                                      // a contract failure is still a pack
        if (out) {
            auto fault = out.get_str("$fault");
            CHECK(fault && *fault == "missing_input");
            CHECK(!out.get_i64(bkeys::kBlobCount));              // no results on the failure pack
        }
    }

    // -----------------------------------------------------------------------
    SECTION("(5) fail-closed edges: miss / no door / empty input / older host");
    {
        auto in = own_pack(build_square_pack(fi), fi);

        CHECK(!xi::use("no_such_instance").process(in).valid()); // -1: unknown name
        CHECK(!xi::use("cam0").process(in).valid());             // -4: source, no pack door

        xi::ScriptPack empty;                                    // absence propagates
        CHECK(!xi::use("det0").process(empty).valid());

        void* saved = g_use_pack_process_fn_;                    // older host: callback never wired
        g_use_pack_process_fn_ = nullptr;
        CHECK(!xi::use("det0").process(in).valid());
        g_use_pack_process_fn_ = saved;

        // The happy path still works after all the failure probes.
        CHECK(xi::use("det0").process(in).valid());
    }

    // -----------------------------------------------------------------------
    SECTION("(7) U1 fault round-trip: ScriptPackBuilder::fault()/src() -> readable fault pack");
    {
        xi::ScriptPackBuilder b;
        CHECK(b.valid());
        CHECK(b.fault("frame_timeout", "gray", "sensor dropped the frame"));
        CHECK(b.src("qa_src"));
        CHECK(b.add_i64("$seq", 41));
        auto f = b.seal();
        CHECK(f.valid());
        CHECK(f.is_fault());
        CHECK(f.fault_reason() && *f.fault_reason() == "frame_timeout");
        CHECK(f.fault_key() && *f.fault_key() == "gray");
        CHECK(f.fault_detail() && *f.fault_detail() == "sensor dropped the frame");
        CHECK(f.src() && *f.src() == "qa_src");
        CHECK(!f.prov());                                    // origin: no chain yet
    }

    // -----------------------------------------------------------------------
    SECTION("(8) U1 short-circuit: fault input never enters the door; reason + $seq carried, hop appended");
    {
        // An OTHERWISE-PROCESSABLE payload (the happy-path square + threshold)
        // poisoned with $fault: if the door ran, blob_count would appear.
        std::vector<uint8_t> gray(16 * 16, 0);
        for (int y = 5; y < 10; ++y)
            for (int x = 5; x < 10; ++x) gray[(size_t)y * 16 + x] = 255;
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_image(b, bkeys::kGray, 16, 16, 1, gray.data());
        fi->builder_add_i64(b, bkeys::kThreshold, 128);
        fi->builder_add_i64(b, bkeys::kMinArea, 1);
        fi->builder_add_str(b, "$fault", "upstream_timeout", 16);
        fi->builder_add_i64(b, "$seq", 7);
        auto in = own_pack(fi->builder_seal(b), fi);

        int calls_before = g_door_calls;
        auto out = xi::use("det0").process(in);
        CHECK(g_door_calls == calls_before);                 // the plugin NEVER ran
        CHECK(out.valid());                                  // poison flows, never empties
        CHECK(out.is_fault());
        CHECK(out.fault_reason() && *out.fault_reason() == "upstream_timeout");
        CHECK(!out.get_i64(bkeys::kBlobCount));              // no results were computed
        CHECK(!out.get_image(bkeys::kGray));                 // the poisoned payload is NOT carried
        CHECK(out.get_i64("$seq").value_or(-1) == 7);        // frame identity rides along
        CHECK(out.src() && *out.src() == "det0");            // the skipped hop owns the result
        CHECK(out.prov() && *out.prov() == "det0");

        // Poison propagates even through a NAME MISS (mirrors the Record NA
        // short-circuit running before instance resolution).
        auto miss = xi::use("no_such_instance").process(in);
        CHECK(miss.valid() && miss.is_fault());
        CHECK(miss.prov() && *miss.prov() == "no_such_instance");
    }

    // -----------------------------------------------------------------------
    SECTION("(9) U1 provenance chain across two chained doors");
    {
        // Happy-path chain: det0's result (a REAL door run) feeds det1. det1's
        // door runs and faults on contract (no 'gray' in det0's output) — a
        // PLUGIN-MINTED fault, auto-stamped by the door glue: $src = det1,
        // $prov = det0's chain + det1.
        auto in   = own_pack(build_square_pack(fi), fi);
        int calls_before = g_door_calls;
        auto out0 = xi::use("det0").process(in);
        CHECK(out0.valid() && !out0.is_fault());
        CHECK(out0.src() && *out0.src() == "det0");          // glue-stamped producer id
        CHECK(out0.prov() && *out0.prov() == "det0");        // unattributed input -> chain starts here
        auto out1 = xi::use("det1").process(out0);
        CHECK(g_door_calls == calls_before + 2);             // BOTH doors genuinely ran
        CHECK(out1.valid() && out1.is_fault());              // contract fault: no 'gray' entry
        CHECK(out1.fault_reason() && *out1.fault_reason() == "missing_input");
        CHECK(out1.src() && *out1.src() == "det1");
        CHECK(out1.prov() && *out1.prov() == "det0/det1");   // the chain accumulated

        // Fault chain: the same poison short-circuited through BOTH hops —
        // neither door runs, the chain still accumulates hop by hop.
        xi_pack_builder fb = fi->builder_new();
        fi->builder_add_str(fb, "$fault", "upstream_timeout", 16);
        auto poison = own_pack(fi->builder_seal(fb), fi);
        calls_before = g_door_calls;
        auto hop1 = xi::use("det0").process(poison);
        auto hop2 = xi::use("det1").process(hop1);
        CHECK(g_door_calls == calls_before);                 // zero door entries
        CHECK(hop2.valid() && hop2.is_fault());
        CHECK(hop2.fault_reason() && *hop2.fault_reason() == "upstream_timeout");
        CHECK(hop2.src() && *hop2.src() == "det1");
        CHECK(hop2.prov() && *hop2.prov() == "det0/det1");
    }

    // -----------------------------------------------------------------------
    SECTION("(10) U1 non-fault packs unaffected: results intact, no $fault, provenance stamped");
    {
        auto in  = own_pack(build_square_pack(fi), fi);
        auto out = xi::use("det0").process(in);
        CHECK(out.valid());
        CHECK(!out.is_fault());
        CHECK(!out.fault_reason());
        CHECK(out.get_i64(bkeys::kBlobCount).value_or(-1) == 1);   // the section-1 result, still
        CHECK(out.get_i64(bkeys::kThresholdUsed).value_or(-1) == 128);
        CHECK(out.src() && *out.src() == "det0");
        CHECK(out.prov() && *out.prov() == "det0");
    }

    // -----------------------------------------------------------------------
    SECTION("(11) balance: every pack + pooled image accounted for");
    CHECK(xi::PackRegistry::instance().live_frames() == base_frames);
    CHECK(pool_live() == base_pool);

    // Teardown: adapters (plugin destroy) BEFORE FreeLibrary.
    xi::InstanceRegistry::instance().clear();
    FreeLibrary(blob_dll);
    FreeLibrary(mock_dll);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
