//
// test_cap_plane.cpp — the capability plane pilot (docs/new_gen/14): host
// registry + forwarding funnel + reentrancy guard, driven end to end against
// the REAL built lib plugin (cap_test_plugin.dll) through the REAL
// CAbiInstanceAdapter — the same mechanics the backend service runs.
//
// Covers the pilot's proof list:
//   1. get_interface publishes xi.cap@1 + xi.cap.provider@1 (zero host_api
//      slots; wrong versions answer NULL).
//   2. Registration discipline: factory registration attributed to the
//      instance; no-owner-context refused; data-plane (process door / cap
//      handler) registration refused; same-owner duplicate overwrites; a name
//      held by ANOTHER instance is refused (ETAKEN) and that loser's destroy
//      sweep does NOT touch the holder's registrations.
//   3. Call round trip + the name-only registry's $v/$probe convention:
//      absent $v = provider default, $v==1 ok, unsupported $v = sealed $fault
//      pack naming the supported range, $probe answers $versions with no work.
//   4. Unknown capability → XI_CAP_EUNKNOWN (-1).
//   5. Handler crash → XI_CAP_ECRASHED (-2), fault charged to the LIB
//      instance: on_fault=refuse quarantines it (health overlay Failed/
//      quarantined) and the NEXT call fails fast -3 without entering plugin
//      code; on_fault=reuse leaves it in service.
//   6. Reentrancy -5 in BOTH directions: a handler calling back up its own
//      chain (test.reenter → test.echo on the same instance), and a consumer
//      calling a capability its own instance provides (simulated with an
//      OwnerGuard around the call).
//   7. Targeted unregister from lifecycle code, and the unregister-on-destroy
//      OWNER SWEEP: destroying the adapter drops every remaining registration.
//   8. Pack-registry balance: every sealed answer released, live_frames back
//      to baseline.
//
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_fault_policy.hpp>
#include <xi/xi_health.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifndef CAP_TEST_PLUGIN_DLL
#define CAP_TEST_PLUGIN_DLL "cap_test_plugin.dll"
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

// Pull an int field out of the plugin's exchange JSON.
static int jint(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return -12345;
    p = s.find(':', p);
    if (p == std::string::npos) return -12345;
    return std::atoi(s.c_str() + p + 1);
}

// Read a str entry out of a sealed pack ("" = absent).
static std::string pstr(const xi_pack_v1* pk, xi_pack_handle h, const char* key) {
    const char* p = nullptr; int32_t n = 0;
    if (h != XI_PACK_NULL && pk->get_str(h, key, &p, &n) && p)
        return std::string(p, (size_t)n);
    return "";
}
static int64_t pi64(const xi_pack_v1* pk, xi_pack_handle h, const char* key, int64_t d) {
    int64_t v = d;
    if (h != XI_PACK_NULL) pk->get_i64(h, key, &v);
    return v;
}

int main() {
    std::printf("[test] capability plane pilot (doc 14)\n");
    xi::install_seh_translator();   // this thread's SEH → catchable seh_exception

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();

    // -----------------------------------------------------------------------
    SECTION("get_interface publishes both plane vtables (zero host_api slots)");
    CHECK(host.get_interface != nullptr);
    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    const auto* prov = static_cast<const xi_cap_provider_v1*>(
        host.get_interface("xi.cap.provider", 1));
    const auto* pk = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(cap && cap->call && cap->available);
    CHECK(prov && prov->register_capability && prov->unregister_capability);
    CHECK(pk != nullptr);
    CHECK(host.get_interface("xi.cap", 2) == nullptr);           // only @1 published
    CHECK(host.get_interface("xi.cap.provider", 0) == nullptr);
    if (!cap || !prov || !pk) return 1;

    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    // -----------------------------------------------------------------------
    SECTION("registration outside any plugin lifecycle context is refused");
    // This host thread carries no owner context — the registry must refuse.
    CHECK(prov->register_capability("test.rogue", nullptr, nullptr) == XI_CAP_REG_EINVAL);
    CHECK(prov->register_capability("", (xi_cap_handler_fn)0x1, nullptr) == XI_CAP_REG_EINVAL);
    CHECK(prov->register_capability("test.rogue", (xi_cap_handler_fn)0x1, nullptr)
          == XI_CAP_REG_ECONTEXT);
    CHECK(cap->available("test.rogue") == 0);

    // -----------------------------------------------------------------------
    SECTION("lib plugin registers in its factory (attributed via owner context)");
    HMODULE dll = LoadLibraryA(CAP_TEST_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n",
                     CAP_TEST_PLUGIN_DLL, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(
        GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) return 1;

    // Mirror the PM's create path exactly: factory under a pre-allocated owner
    // scope, adapter adopts the id (xi_pm_load.hpp / ImagePoolOwnerScope).
    auto make_lib = [&](const char* name) -> std::shared_ptr<xi::CAbiInstanceAdapter> {
        xi::ImagePoolOwnerScope scope;
        void* raw = scope.run_factory([&] { return create(&host, name); });
        auto ad = std::make_shared<xi::CAbiInstanceAdapter>(
            name, "cap_test_plugin", dll, raw, /*reentrant=*/true, /*max_conc=*/0);
        ad->adopt_owner_id(scope.release());
        xi::InstanceRegistry::instance().add(ad);
        return ad;
    };

    auto lib = make_lib("lib");
    {
        std::string s = lib->exchange("{\"command\":\"status\"}");
        CHECK(jint(s, "reg_echo")    == XI_CAP_REG_OK);
        CHECK(jint(s, "reg_ver")     == XI_CAP_REG_OK);
        CHECK(jint(s, "reg_crash")   == XI_CAP_REG_OK);
        CHECK(jint(s, "reg_reenter") == XI_CAP_REG_OK);
        // Same-owner duplicate re-register = overwrite (the reinit path): OK.
        CHECK(jint(s, "reg_dup")     == XI_CAP_REG_OK);
    }
    CHECK(cap->available("test.echo") == 1);
    CHECK(cap->available("test.ver") == 1);

    // -----------------------------------------------------------------------
    SECTION("call round trip through the funnel");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 41);
        xi_pack_handle in = pk->builder_seal(b);
        CHECK(in != XI_PACK_NULL);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_OK);
        CHECK(out != XI_PACK_NULL);
        CHECK(pi64(pk, out, "x_echo", -1) == 42);
        if (out != XI_PACK_NULL) pk->release(out);
        pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("unknown capability -> XI_CAP_EUNKNOWN, bad args fail closed");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.nope", in, &out) == XI_CAP_EUNKNOWN);
        CHECK(out == XI_PACK_NULL);
        CHECK(cap->call(nullptr, in, &out) == XI_CAP_EUNKNOWN);
        CHECK(cap->call("test.echo", XI_PACK_NULL, &out) == XI_CAP_EUNKNOWN);
        CHECK(cap->call("test.echo", in, nullptr) == XI_CAP_EUNKNOWN);
        pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("name-only registry: the $v / $probe in-pack version convention");
    {
        // absent $v -> the provider's documented default (1).
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "payload", 7);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.ver", in, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "v_used", -1) == 1);
        CHECK(pstr(pk, out, "$fault").empty());
        pk->release(out); pk->release(in);

        // $v == 1 -> served.
        b = pk->builder_new();
        pk->builder_add_i64(b, "$v", 1);
        in = pk->builder_seal(b);
        CHECK(cap->call("test.ver", in, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "v_used", -1) == 1);
        pk->release(out); pk->release(in);

        // unsupported $v -> a NORMAL sealed $fault pack naming the range
        // (transport rc stays OK — versioning is capability contract, not
        // funnel verdict).
        b = pk->builder_new();
        pk->builder_add_i64(b, "$v", 99);
        in = pk->builder_seal(b);
        CHECK(cap->call("test.ver", in, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "unsupported_version");
        CHECK(pstr(pk, out, "$versions") == "1");
        pk->release(out); pk->release(in);

        // $probe: true -> supported versions, no work done.
        b = pk->builder_new();
        if (pk->builder_add_bool) pk->builder_add_bool(b, "$probe", 1);
        else                      pk->builder_add_i64(b, "$probe", 1);
        in = pk->builder_seal(b);
        CHECK(cap->call("test.ver", in, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$versions") == "1");
        pk->release(out); pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("registration from the data-plane door is refused (lifecycle only)");
    {
        xi_pack_handle out = lib->run_pack_door(XI_PACK_NULL);  // door carries DataPlaneMark
        if (out != XI_PACK_NULL) pk->release(out);
        std::string s = lib->exchange("{\"command\":\"status\"}");
        CHECK(jint(s, "reg_in_process") == XI_CAP_REG_ECONTEXT);
        CHECK(cap->available("test.from_process") == 0);
    }

    // -----------------------------------------------------------------------
    SECTION("reentrancy -5, direction 1: handler calls back up its own chain");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.reenter", in, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "reenter_rc", 0) == XI_CAP_EREENTRY);
        // ...and a handler is data plane: registering inside it is refused.
        CHECK(pi64(pk, out, "reg_in_handler_rc", 0) == XI_CAP_REG_ECONTEXT);
        CHECK(cap->available("test.sneaky") == 0);
        pk->release(out); pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("reentrancy -5, direction 2: a plugin calling a capability its own "
            "instance provides");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        // Simulate the consumer position: this thread is "inside" lib's own
        // entry point (the owner context every adapter call sets).
        {
            xi::ImagePool::OwnerGuard g(lib->owner_id());
            CHECK(cap->call("test.echo", in, &out) == XI_CAP_EREENTRY);
            CHECK(out == XI_PACK_NULL);
        }
        // Outside the guard the same call succeeds.
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_OK);
        pk->release(out); pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("handler crash -> -2, charged to the LIB instance (on_fault=refuse "
            "quarantines; next call fails fast -3)");
    {
        lib->set_on_fault(xi::OnFault::Refuse);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.crash", in, &out) == XI_CAP_ECRASHED);
        CHECK(out == XI_PACK_NULL);
        CHECK(lib->quarantined());
        // Health overlay: the funnel marked the LIB instance failed/quarantined.
        {
            xi::CompHealth h{}; std::string reason; int64_t since = 0;
            CHECK(xi::health().instance_fault("lib", h, reason, since));
            CHECK(h == xi::CompHealth::Failed);
            CHECK(reason == xi::kReasonQuarantined);
        }
        // Fail-fast without entering plugin code — even for a healthy capability.
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_EQUARANTINED);
        CHECK(out == XI_PACK_NULL);
        // Re-enable (the operator's re-commit path) — back in service.
        lib->set_quarantined(false);
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_OK);
        pk->release(out); pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("on_fault=reuse: a crash answers -2 but the instance stays in service");
    {
        lib->set_on_fault(xi::OnFault::Reuse);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 5);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.crash", in, &out) == XI_CAP_ECRASHED);
        CHECK(!lib->quarantined());
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "x_echo", -1) == 6);
        pk->release(out); pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("a second instance cannot steal a registered name (ETAKEN), and its "
            "destroy sweep leaves the holder untouched");
    {
        auto lib2 = make_lib("lib2");
        std::string s = lib2->exchange("{\"command\":\"status\"}");
        CHECK(jint(s, "reg_echo")    == XI_CAP_REG_ETAKEN);
        CHECK(jint(s, "reg_ver")     == XI_CAP_REG_ETAKEN);
        CHECK(jint(s, "reg_crash")   == XI_CAP_REG_ETAKEN);
        CHECK(jint(s, "reg_reenter") == XI_CAP_REG_ETAKEN);
        xi::InstanceRegistry::instance().remove("lib2");
        lib2.reset();                                    // dtor sweep (owns nothing)
        // The holder's capabilities survived the loser's sweep (owner-keyed).
        CHECK(cap->available("test.echo") == 1);
        CHECK(cap->available("test.ver") == 1);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 9);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "x_echo", -1) == 10);
        pk->release(out); pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("targeted unregister from lifecycle code (exchange)");
    {
        std::string s = lib->exchange("{\"command\":\"unregister_echo\"}");
        CHECK(jint(s, "unreg_echo") == XI_CAP_REG_OK);
        CHECK(cap->available("test.echo") == 0);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.echo", in, &out) == XI_CAP_EUNKNOWN);
        pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("unregister-on-destroy: the adapter dtor sweep drops every "
            "remaining registration");
    {
        CHECK(cap->available("test.ver") == 1);          // still registered
        CHECK(cap->available("test.crash") == 1);
        CHECK(cap->available("test.reenter") == 1);
        xi::InstanceRegistry::instance().remove("lib");
        lib.reset();                                     // adapter dtor -> sweep
        CHECK(cap->available("test.ver") == 0);
        CHECK(cap->available("test.crash") == 0);
        CHECK(cap->available("test.reenter") == 0);
        CHECK(xi::CapRegistry::instance().size() == 0);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("test.ver", in, &out) == XI_CAP_EUNKNOWN);
        pk->release(in);
    }

    // -----------------------------------------------------------------------
    SECTION("pack-registry balance: every sealed answer was released");
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    if (g_failures == 0) {
        std::printf("\n[test] capability plane pilot: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n[test] capability plane pilot: %d FAILURE(S)\n", g_failures);
    return 1;
}
