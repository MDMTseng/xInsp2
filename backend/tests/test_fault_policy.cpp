//
// test_fault_policy.cpp — the post-fault quarantine policy (adoption-map item 14).
//
// Drives the REAL CAbiInstanceAdapter + a REAL crashing plugin (fault_plugin.dll)
// through the mechanics the fault boundary (service_sinks.cpp use_process_inline_)
// relies on, plus the health-contract overlay it drives. A hardware fault inside
// the plugin's process() propagates to the host's SEH boundary here exactly as it
// does in the backend, so the caught-fault path is genuine, not simulated.
//
// The three service-side policy branches are tiny (a switch that calls the adapter
// primitives + the health registry — see apply_on_fault_policy_ /
// apply_pending_reinit_ / quarantine_instance_); those handlers are file-static in
// the service, so this test replays that SAME logic against the real primitives to
// assert the composed behavior end to end:
//
//   * reuse  — default: nothing changes, the instance stays in service.
//   * reinit — a caught fault rebuilds the instance (new serial, dropped in-flight
//              state, restored committed config, no re-crash) and clears the health
//              overlay; N consecutive rebuild failures escalate to refuse.
//   * refuse — a caught fault quarantines the instance (fail-fast gate + health
//              failed/quarantined); re-committing config re-enables it.
//
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_fault_policy.hpp>
#include <xi/xi_health.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef FAULT_PLUGIN_DLL
#define FAULT_PLUGIN_DLL "fault_plugin.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::fprintf(stderr, "\n[section] %s\n", name)

// Pull an int field out of the plugin's exchange JSON ({"serial":..,"scratch":..}).
static int jint(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return -12345;
    p = s.find(':', p);
    if (p == std::string::npos) return -12345;
    return std::atoi(s.c_str() + p + 1);
}

// Run one process() call; returns true if it faulted (caught at the SEH boundary,
// exactly as use_process_inline_ does).
static bool process_caught_fault(xi::CAbiInstanceAdapter& ad) {
    const char* empty = "{}";
    xi_record in{};
    in.data = reinterpret_cast<const uint8_t*>(empty); in.len = 2;
    xi_record_out out; xi_record_out_init(&out);
    bool faulted = false;
    try {
        ad.process(&in, &out);
    } catch (const xi::seh_exception&) {
        faulted = true;
    }
    xi_record_out_free(&out);
    return faulted;
}

int main() {
    std::printf("[test] item 14 — post-fault quarantine policy\n");
    xi::install_seh_translator();   // this thread's SEH → catchable seh_exception

    HMODULE dll = LoadLibraryA(FAULT_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", FAULT_PLUGIN_DLL, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(
        GetProcAddress(dll, "xi_plugin_create"));
    auto create_null = reinterpret_cast<xi::PluginInfo::CFactoryFn>(
        GetProcAddress(dll, "xi_plugin_create_null"));
    CHECK(create && create_null);
    if (!create) { FreeLibrary(dll); return 1; }
    static xi_host_api host = xi::ImagePool::make_host_api();

    // Build a live adapter over a fresh plugin instance, wired like the PM does.
    auto make_adapter = [&](const char* name, xi::OnFault policy) {
        void* raw = create(&host, name);
        auto* ad = new xi::CAbiInstanceAdapter(name, "fault_plugin", dll, raw,
                                               /*reentrant=*/false, /*max_conc=*/0);
        ad->set_on_fault(policy);
        ad->arm_reinit(create, &host);
        return ad;
    };

    // -----------------------------------------------------------------------
    SECTION("policy vocabulary parses + names round-trip");
    CHECK(xi::parse_on_fault("reuse")  == xi::OnFault::Reuse);
    CHECK(xi::parse_on_fault("reinit") == xi::OnFault::Reinit);
    CHECK(xi::parse_on_fault("refuse") == xi::OnFault::Refuse);
    CHECK(xi::parse_on_fault("")       == xi::OnFault::Reuse);   // absent → default
    CHECK(xi::parse_on_fault("bogus", xi::OnFault::Refuse) == xi::OnFault::Refuse); // typo → caller default
    CHECK(std::string(xi::on_fault_name(xi::OnFault::Reinit)) == "reinit");
    CHECK(xi::kReinitEscalateAfter >= 1);

    // -----------------------------------------------------------------------
    SECTION("reuse (default): a caught fault leaves the instance in service");
    {
        xi::CAbiInstanceAdapter* ad = make_adapter("reuse0", xi::OnFault::Reuse);
        ad->exchange("{\"command\":\"arm_crash\"}");
        CHECK(process_caught_fault(*ad));       // it faulted, and we caught it
        // reuse policy: no rebuild requested, not quarantined — same instance stays.
        CHECK(!ad->reinit_pending());
        CHECK(!ad->quarantined());
        delete ad;
    }

    // -----------------------------------------------------------------------
    SECTION("reinit: a caught fault rebuilds the instance (drops in-flight state, "
            "restores committed config, no re-crash)");
    {
        xi::CAbiInstanceAdapter* ad = make_adapter("reinit0", xi::OnFault::Reinit);
        ad->set_def("{\"value\":5}");           // committed config = value 5
        ad->exchange("{\"command\":\"bump\"}"); // in-flight scratch = 1
        std::string before = ad->exchange("{\"command\":\"stats\"}");
        int serial0 = jint(before, "serial");
        CHECK(jint(before, "value") == 5);
        CHECK(jint(before, "scratch") == 1);

        ad->exchange("{\"command\":\"arm_crash\"}");
        CHECK(process_caught_fault(*ad));
        // Policy (reinit): request a rebuild before next use.
        CHECK(ad->on_fault() == xi::OnFault::Reinit);
        ad->request_reinit();
        CHECK(ad->reinit_pending());

        // apply_pending_reinit_: rebuild succeeds → overlay clears, counter resets.
        bool ok = ad->reinit();
        CHECK(ok);
        CHECK(!ad->reinit_pending());           // reinit() consumes the request
        ad->reset_reinit_fails();

        std::string after = ad->exchange("{\"command\":\"stats\"}");
        CHECK(jint(after, "serial") != serial0);   // a genuinely NEW instance
        CHECK(jint(after, "value") == 5);          // committed config restored
        CHECK(jint(after, "scratch") == 0);        // in-flight state dropped
        CHECK(jint(after, "armed") == 0);          // crash arming dropped
        CHECK(!process_caught_fault(*ad));         // rebuilt instance no longer crashes
        delete ad;
    }

    // -----------------------------------------------------------------------
    SECTION("reinit escalates to refuse after kReinitEscalateAfter failures");
    {
        xi::CAbiInstanceAdapter* ad = make_adapter("reinit_fail0", xi::OnFault::Reinit);
        ad->arm_reinit(create_null, &host);     // every rebuild now FAILS (factory → null)
        for (int attempt = 1; attempt <= xi::kReinitEscalateAfter; ++attempt) {
            CHECK(!ad->quarantined());           // not yet pulled from service
            ad->request_reinit();
            // apply_pending_reinit_: a failed rebuild bumps the counter; the Nth
            // failure escalates to refuse (quarantine).
            bool ok = ad->reinit();
            CHECK(!ok);
            if (ad->note_reinit_fail() >= xi::kReinitEscalateAfter)
                ad->set_quarantined(true);
        }
        CHECK(ad->quarantined());                // escalated to refuse
        delete ad;
    }

    // -----------------------------------------------------------------------
    SECTION("refuse: a caught fault quarantines (fail-fast gate + health "
            "failed/quarantined); re-committing config re-enables");
    {
        auto& H = xi::health();
        H.reset_for_test();
        H.set_state(xi::SysState::Running);

        xi::CAbiInstanceAdapter* ad = make_adapter("refuse0", xi::OnFault::Refuse);
        ad->exchange("{\"command\":\"arm_crash\"}");
        CHECK(process_caught_fault(*ad));
        // Policy (refuse): quarantine now — set the fail-fast gate + mark the health
        // contract failed/quarantined (quarantine_instance_).
        CHECK(ad->on_fault() == xi::OnFault::Refuse);
        ad->set_quarantined(true);
        H.mark_instance_fault("refuse0", xi::CompHealth::Failed, xi::kReasonQuarantined);

        // The fail-fast gate use_process_inline_ reads: quarantined() is true, so a
        // subsequent call returns the structured refuse (rc -3) WITHOUT entering
        // plugin code — which is why a quarantined instance can't re-crash.
        CHECK(ad->quarantined());
        { xi::CompHealth h; std::string r; int64_t s = 0;
          CHECK(H.instance_fault("refuse0", h, r, s));
          CHECK(h == xi::CompHealth::Failed);
          CHECK(r == "quarantined"); }
        CHECK(H.state() == xi::SysState::Degraded);   // a quarantined component ⇒ degraded top

        // Re-enable surface: re-committing config (set_inst_state Active) lifts the
        // gate + clears the overlay.
        ad->set_quarantined(false);
        ad->reset_reinit_fails();
        H.clear_instance_degraded("refuse0");
        CHECK(!ad->quarantined());
        CHECK(H.state() == xi::SysState::Running);
        { xi::CompHealth h; std::string r; int64_t s = 0;
          CHECK(!H.instance_fault("refuse0", h, r, s)); }
        delete ad;
    }

    FreeLibrary(dll);
    if (g_failures) { std::fprintf(stderr, "\n%d CHECK(s) FAILED\n", g_failures); return 1; }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
