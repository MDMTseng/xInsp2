//
// test_crash_leak_counter.cpp — Q0f: the deliberate per-crash doc leak is OBSERVABLE.
//
// When a plugin's process() is caught crashing mid-call (rc -2) on a path that
// handed the plugin a BORROWED doc with a reserved adopter ref, the host does NOT
// release that ref: a torn callee's ownership state is unknowable, so releasing
// risks a use-after-free / double-free. The host leaks the ref instead
// (leak-over-UAF). That leak was invisible in production; Q0f makes it countable via
// DocRegistry::note_crash_leak() → dispatch_stats.crash_leaked_docs_lifetime.
//
// This test drives the REAL leak site end to end: the actual xi_use.hpp UseProxy
// (which reserves the ref through share_out and, on rc -2, deliberately declines to
// release it — the genuine leak) against a REAL crashing plugin (fault_plugin.dll,
// raw exports so the ACCESS_VIOLATION propagates to the host SEH boundary exactly as
// in the backend). The installed callback mirrors service_sinks.cpp
// use_process_inline_'s borrowed-doc branch: hand over the borrowed doc, run
// process() under the SEH boundary, and on a caught crash return -2 while calling the
// REAL DocRegistry::note_crash_leak() — the same host funnel every service-side
// use().process() crash flows through. We then assert BOTH that the counter ticked
// AND that a host-owned doc genuinely leaked (DocRegistry::live_count rose and stays
// risen), i.e. the counter is measuring a real leak, not a phantom.
//
#include <xi/xi_use.hpp>          // xi::use / UseProxy (the real leak site) + Record
#include <xi/xi_image_pool.hpp>   // make_host_api (doc_retain/doc_release → DocRegistry)
#include <xi/xi_doc_registry.hpp> // DocRegistry: crash_leaked_lifetime / live_count
#include <xi/xi_cabi_adapter.hpp> // CAbiInstanceAdapter (drives the real plugin)
#include <xi/xi_seh.hpp>          // install_seh_translator + seh_exception
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <string>

#ifndef FAULT_PLUGIN_DLL
#define FAULT_PLUGIN_DLL "fault_plugin.dll"
#endif

// ---- harness (inline CHECK/SECTION, matching the neighboring test_*.cpp) ----
static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

// xi_use.hpp declares these extern (the script DLL defines them static via
// xi_script_support.hpp). This host-role test OWNS the storage and points the use
// callbacks at its own crashing target — exactly as test_parallel_safety.cpp does.
void* g_use_process_fn_     = nullptr;
void* g_use_exchange_fn_    = nullptr;
void* g_use_grab_fn_        = nullptr;
void* g_use_host_api_       = nullptr;
void* g_trigger_info_fn_    = nullptr;
void* g_trigger_image_fn_   = nullptr;
void* g_trigger_sources_fn_ = nullptr;
void* g_trigger_leader_fn_  = nullptr;
void* g_trigger_meta_fn_    = nullptr;

// One host_api over the live singleton ImagePool + DocRegistry — the same table the
// backend hands a script (doc_retain/doc_release route to the real DocRegistry, so
// UseProxy::share_out reserves a real, countable ref).
static const xi_host_api& host_api() {
    static xi_host_api api = xi::ImagePool::make_host_api();
    return api;
}

// The crashing target the installed use-process callback drives.
static xi::CAbiInstanceAdapter* g_ad = nullptr;

// Mirror of service_sinks.cpp use_process_inline_'s BORROWED-doc branch + crash
// catch — the host funnel every doc-carrying use().process() crash passes through.
// fault_plugin exports xi_yyjson_abi with our layout stamp, so the real host would
// also take the borrowed path for it; we model that path directly.
static int leaking_use_process(const char* /*name*/, const void* input_doc,
                               const uint8_t* input_data, int32_t input_len,
                               const xi_record_image* images, int image_count,
                               xi_record_out* output) {
    xi_record in{};
    in.doc         = input_doc;   // borrowed-doc path: reserved adopter ref LEFT UNRELEASED
    in.data        = input_data;
    in.len         = input_len;
    in.images      = images;
    in.image_count = image_count;
    try {
        return g_ad->process(&in, output);
    } catch (const xi::seh_exception&) {
        // Caught crash: the torn callee may or may not have adopted the reserved ref
        // — we don't second-guess it (leak-over-UAF), so it leaks. Count it exactly
        // as the backend does. (input_doc null ⇒ no ref was reserved ⇒ nothing leaked.)
        if (input_doc) xi::DocRegistry::instance().note_crash_leak();
        return -2;
    }
}

int main() {
    std::printf("[test] Q0f — caught-crash doc-leak observability\n");
    xi::install_seh_translator();   // this thread's SEH fault → catchable seh_exception

    HMODULE dll = LoadLibraryA(FAULT_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", FAULT_PLUGIN_DLL, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) { FreeLibrary(dll); return 1; }

    const xi_host_api& host = host_api();
    // Heap-allocated so we can destroy it BEFORE FreeLibrary — the adapter dtor
    // calls the plugin's destroy export, which must still be mapped.
    auto* ad = new xi::CAbiInstanceAdapter("crash0", "fault_plugin", dll,
                                           create(&host, "crash0"),
                                           /*reentrant=*/false, /*max_conc=*/0);
    g_ad = ad;

    // Wire the script-side use() seam at this host test.
    g_use_host_api_   = (void*)&host;
    g_use_process_fn_ = (void*)&leaking_use_process;

    auto& reg = xi::DocRegistry::instance();
    reg.reset_crash_leaked_for_test();

    const uint64_t leaks0 = reg.crash_leaked_lifetime();
    const size_t   live0  = reg.live_count();

    // -----------------------------------------------------------------------
    SECTION("a caught crash on a doc-carrying process() ticks the leak counter "
            "AND genuinely strands one host-owned doc");
    {
        ad->exchange("{\"command\":\"arm_crash\"}");   // next process() faults

        // The REAL UseProxy path: share_out reserves an adopter ref on the input doc,
        // process_fn returns -2 (crash), and xi_use.hpp deliberately leaves the ref.
        xi::Record in;
        in.set("frame", 7);                            // owned doc ⇒ share_out has a ref to reserve
        xi::Record out = xi::use("crash0").process(in);

        // Crash ⇒ empty provenance-tagged result, never the torn output.
        CHECK(out.get_int("frame", -1) == -1);

        // Counted exactly once — this is the value dispatch_stats serializes as
        // crash_leaked_docs_lifetime (service_cmd_observability.cpp).
        CHECK(reg.crash_leaked_lifetime() == leaks0 + 1);
        // The leak is REAL: the reserved ref was never released, so one extra
        // host-owned doc is now registered (and lives on past `in`'s destruction).
        CHECK(reg.live_count() == live0 + 1);
    }
    // `in` is destroyed; its own ref drops, but the leaked reserved ref keeps the doc
    // alive — a genuine leak, not a deferred free.
    CHECK(reg.live_count() == live0 + 1);

    // -----------------------------------------------------------------------
    SECTION("the counter accumulates — the operator's 'one crash' vs 'leaking every N frames' signal");
    {
        const uint64_t leaks1 = reg.crash_leaked_lifetime();
        const size_t   live1  = reg.live_count();

        ad->exchange("{\"command\":\"arm_crash\"}");   // reused instance stays armed
        xi::Record in2;
        in2.set("frame", 8);
        xi::Record out2 = xi::use("crash0").process(in2);
        CHECK(out2.get_int("frame", -1) == -1);

        CHECK(reg.crash_leaked_lifetime() == leaks1 + 1);   // ticked again
        CHECK(reg.live_count() == live1 + 1);               // a second doc genuinely stranded
    }

    // -----------------------------------------------------------------------
    SECTION("process-uptime semantics: cumulative, reset only by an explicit test reset");
    {
        CHECK(reg.crash_leaked_lifetime() == leaks0 + 2);
        reg.reset_crash_leaked_for_test();
        CHECK(reg.crash_leaked_lifetime() == 0);
    }

    // Tear down the seam + adapter BEFORE unloading the plugin DLL.
    g_use_process_fn_ = nullptr;
    g_ad = nullptr;
    delete ad;
    FreeLibrary(dll);
    if (g_failures) { std::fprintf(stderr, "\n%d CHECK(s) FAILED\n", g_failures); return 1; }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
