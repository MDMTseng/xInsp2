//
// test_cap_autoload.cpp — V3 machine-level lib-plugin autoload (docs/new_gen/14
// + doc 19 V3): a capability provider is available WITHOUT any project
// declaring a per-instance. Drives the REAL PluginManager against the REAL
// built lib plugin (cap_test_plugin.dll, marked "lib"+"autoload" via a copied
// plugin.json) — the exact mechanics service_main runs at boot.
//
// Covers the V3 proof list:
//   1. Autoload at boot: scan_plugins + autoload_machine_providers() brings up
//      ONE machine provider; its capabilities register with NO project open —
//      available("test.echo")==1 and the call round-trips.
//   2. Project precedence: creating a PROJECT instance of the same plugin
//      DISPLACES the machine provider (no double-register / ETAKEN) and the
//      capability stays available; removing it REINSTATES the machine provider.
//   3. close_project reinstates a machine provider a project had displaced.
//   4. Crash → quarantine (on_fault=refuse) → machine-scoped recovery via
//      reload_machine_provider(): a fresh factory re-registers, calls work again.
//   5. Teardown sweep: destroying the PluginManager owner-sweeps every machine
//      registration — the registry is clean.
//
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_abi.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef CAP_TEST_PLUGIN_DLL
#define CAP_TEST_PLUGIN_DLL "cap_test_plugin.dll"
#endif

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__,        \
                         __LINE__);                                             \
            ++g_failures;                                                       \
        }                                                                       \
    } while (0)

// Resolve the public host-owned vtables (zero-slot get_interface route). Calling
// through these is exactly what a consumer plugin does; publishing happens the
// first time PluginManager touches default_host_api() (during autoload).
static const xi_cap_v1*  CAP()  { return xi::cap_v1_iface(); }
static const xi_pack_v1* PACK() { return xi::pack_v1_iface(); }

// Call "test.echo" (x -> x+1) through the funnel; returns the funnel rc and, on
// success, writes the echoed value.
static int32_t call_echo(int64_t x, int64_t* echoed) {
    const xi_pack_v1* pk = PACK();
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_i64(b, "x", x);
    xi_pack_handle req = pk->builder_seal(b);
    xi_pack_handle rsp = XI_PACK_NULL;
    int32_t rc = CAP()->call("test.echo", req, &rsp);
    pk->release(req);
    if (rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
        if (echoed) { *echoed = 0; pk->get_i64(rsp, "x_echo", echoed); }
        pk->release(rsp);
    }
    return rc;
}

static int32_t call_crash() {
    const xi_pack_v1* pk = PACK();
    xi_pack_builder b = pk->builder_new();
    pk->builder_add_i64(b, "z", 1);
    xi_pack_handle req = pk->builder_seal(b);
    xi_pack_handle rsp = XI_PACK_NULL;
    int32_t rc = CAP()->call("test.crash", req, &rsp);
    pk->release(req);
    if (rsp != XI_PACK_NULL) pk->release(rsp);
    return rc;
}

static void write_manifest(const fs::path& folder) {
    std::ofstream f((folder / "plugin.json").string());
    f << "{\n"
         "  \"name\": \"autolib\",\n"
         "  \"dll\": \"autolib.dll\",\n"
         "  \"factory\": \"xi_plugin_create\",\n"
         "  \"lib\": true,\n"
         "  \"autoload\": true,\n"
         "  \"on_fault\": \"refuse\",\n"
         "  \"requires\": [ { \"iface\": \"xi.pack\", \"min\": 1 },\n"
         "                  { \"iface\": \"xi.cap.provider\", \"min\": 1 } ]\n"
         "}\n";
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    for (auto& e : v) if (e == s) return true;
    return false;
}

int main() {
    // This thread drives a crashing capability handler directly (section 4), so it
    // must install the fault→seh_exception translator so the cap funnel's
    // catch(seh_exception) fires — same as test_cap_plane. On Windows MSVC /EHa
    // lets the funnel's catch(...) absorb the SEH fault without this; on POSIX the
    // signal-based translator has to be armed explicitly on the faulting thread.
    xi::install_seh_translator();

    // ---- lay out a throwaway plugins dir with the autoload lib plugin --------
    fs::path root = fs::temp_directory_path() /
                    ("xi_cap_autoload_" + std::to_string(::GetCurrentProcessId()));
    fs::path plugdir = root / "plugins";
    fs::path libfolder = plugdir / "autolib";
    fs::path projdir = root / "proj";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(libfolder, ec);
    fs::copy_file(CAP_TEST_PLUGIN_DLL, libfolder / "autolib.dll",
                  fs::copy_options::overwrite_existing, ec);
    CHECK(!ec, "copy cap_test_plugin.dll into the plugins dir");
    write_manifest(libfolder);

    const size_t base_reg = xi::CapRegistry::instance().size();

    {
        xi::PluginManager pm;
        pm.set_autoload_enabled(true);   // deployment opt-in (service: --autoload-lib)
        // QuiesceToken: bare PluginManager in a single-threaded test — no
        // dispatch pool exists (see xi_quiesce_token.hpp).
        int scanned = pm.scan_plugins(xi::QuiesceToken::assert_no_dispatch(), plugdir.string());
        CHECK(scanned == 1, "scan_plugins discovers the autoload lib plugin");

        // ---- 1. autoload at boot: capability live with NO project ------------
        CHECK(CAP()->available("test.echo") == 0,
              "precondition: capability not registered before autoload");
        int made = pm.autoload_machine_providers();
        CHECK(made == 1, "autoload brings up exactly one machine provider");
        CHECK(has(pm.machine_provider_plugins(), "autolib"),
              "the plugin is machine-provided after autoload");
        CHECK(CAP()->available("test.echo") == 1,
              "capability available with NO project instance (the V3 headline)");
        int64_t echoed = -1;
        CHECK(call_echo(41, &echoed) == XI_CAP_OK && echoed == 42,
              "the machine provider serves the call with no project open");

        // Idempotent: a second autoload pass creates nothing new.
        CHECK(pm.autoload_machine_providers() == 0, "autoload is idempotent");

        // ---- 2. project precedence: a project instance displaces the machine
        //         provider (no double-register), and removing it reinstates it --
        // assert_no_dispatch: bare PluginManager under test, no dispatch pool exists.
        CHECK(pm.create_project(xi::QuiesceToken::assert_no_dispatch(), projdir.string(), "p"),
              "create a project");
        std::string err;
        auto* ii = pm.create_instance(xi::QuiesceToken::assert_no_dispatch(), "codec", "autolib", &err);
        CHECK(ii != nullptr,
              ("project instance of the autoload plugin is allowed: " + err).c_str());
        CHECK(!has(pm.machine_provider_plugins(), "autolib"),
              "machine provider is evicted while a project instance provides it");
        CHECK(CAP()->available("test.echo") == 1,
              "capability still available — the project instance registered it");
        echoed = -1;
        CHECK(call_echo(7, &echoed) == XI_CAP_OK && echoed == 8,
              "the project instance now serves the call");

        CHECK(pm.remove_instance(xi::QuiesceToken::assert_no_dispatch(), "codec", /*delete_folder=*/true),
              "remove the project instance");
        CHECK(has(pm.machine_provider_plugins(), "autolib"),
              "machine provider reinstated once the project instance is gone");
        CHECK(CAP()->available("test.echo") == 1,
              "capability stays available across the project->machine handoff");

        // ---- 3. close_project reinstates a displaced machine provider --------
        // Re-declare a project instance, then close: the machine provider (whose
        // global DLL is still mapped) must step back in.
        ii = pm.create_instance(xi::QuiesceToken::assert_no_dispatch(), "codec2", "autolib", &err);
        CHECK(ii != nullptr, "second project instance created");
        CHECK(!has(pm.machine_provider_plugins(), "autolib"),
              "machine provider evicted again by the second project instance");
        pm.close_project(xi::QuiesceToken::assert_no_dispatch());
        CHECK(has(pm.machine_provider_plugins(), "autolib"),
              "close_project reinstates the machine provider");
        echoed = -1;
        CHECK(call_echo(99, &echoed) == XI_CAP_OK && echoed == 100,
              "machine provider serves again after close_project");

        // ---- 4. crash -> quarantine -> machine-scoped recovery ---------------
        CHECK(call_crash() == XI_CAP_ECRASHED,
              "a crashing handler is charged to the lib instance (-2)");
        CHECK(call_echo(1, nullptr) == XI_CAP_EQUARANTINED,
              "on_fault=refuse quarantines it; the next call fails fast (-3)");
        // assert_no_dispatch: bare PluginManager under test, no dispatch pool exists.
        CHECK(pm.reload_machine_provider(xi::QuiesceToken::assert_no_dispatch(), "autolib"),
              "machine-scoped recovery: reload the provider from a fresh factory");
        echoed = -1;
        CHECK(call_echo(5, &echoed) == XI_CAP_OK && echoed == 6,
              "the recovered machine provider serves calls again");

        // ---- 5. teardown sweep: pm destruction drops all registrations -------
    }
    CHECK(CAP()->available("test.echo") == 0,
          "teardown sweep: no capability survives the PluginManager");
    CHECK(xi::CapRegistry::instance().size() == base_reg,
          "teardown sweep: the capability registry is back to baseline");

    fs::remove_all(root, ec);

    if (g_failures) {
        std::fprintf(stderr, "test_cap_autoload: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "test_cap_autoload: OK — machine autoload, project "
                         "precedence, quarantine recovery, teardown sweep\n");
    return 0;
}
