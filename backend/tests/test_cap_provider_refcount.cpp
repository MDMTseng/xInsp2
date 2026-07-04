//
// test_cap_provider_refcount.cpp — B1 regression (red-team hardening): a second
// project instance of the SAME capability-provider plugin must not leave the
// capability permanently gone when the first instance is removed.
//
// Before the fix, CapRegistry refused the 2nd registrant with XI_CAP_REG_ETAKEN
// and FORGOT it: the 2nd instance was live-but-unregistered, and removing the
// 1st instance swept the name, so a live provider was left with NO registration
// and the capability resolved to nothing forever.
//
// The fix ref-counts the name: the losing registration is remembered as a
// shadow and PROMOTED when the active holder is removed. The ETAKEN return to
// the loser is preserved (provider identity is configuration, not a race), so
// test_cap_plane's contract is untouched — this test proves the promotion.
//
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_abi.h>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
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

static int jint(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    auto p = s.find(pat);
    if (p == std::string::npos) return -12345;
    p = s.find(':', p);
    if (p == std::string::npos) return -12345;
    return std::atoi(s.c_str() + p + 1);
}
static int64_t pi64(const xi_pack_v1* pk, xi_pack_handle h, const char* key, int64_t d) {
    int64_t v = d;
    if (h != XI_PACK_NULL) pk->get_i64(h, key, &v);
    return v;
}

int main() {
    std::printf("[test] cap provider ref-count (B1 regression)\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();

    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    const auto* pk  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    CHECK(cap && pk);
    if (!cap || !pk) return 1;

    HMODULE dll = LoadLibraryA(CAP_TEST_PLUGIN_DLL);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", CAP_TEST_PLUGIN_DLL, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) return 1;

    // Mirror the PM create path: factory under an owner scope, adapter adopts it.
    auto make_lib = [&](const char* name) -> std::shared_ptr<xi::CAbiInstanceAdapter> {
        xi::ImagePoolOwnerScope scope;
        void* raw = scope.run_factory([&] { return create(&host, name); });
        auto ad = std::make_shared<xi::CAbiInstanceAdapter>(
            name, "cap_test_plugin", dll, raw, /*reentrant=*/true, /*max_conc=*/0);
        ad->adopt_owner_id(scope.release());
        xi::InstanceRegistry::instance().add(ad);
        return ad;
    };

    auto call_echo = [&](int64_t x) -> int64_t {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", x);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        int32_t rc = cap->call("test.echo", in, &out);
        int64_t r = (rc == XI_CAP_OK) ? pi64(pk, out, "x_echo", -1) : -999;
        if (out != XI_PACK_NULL) pk->release(out);
        pk->release(in);
        return r;
    };

    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    // -----------------------------------------------------------------------
    SECTION("first provider instance holds test.echo (primary)");
    auto provA = make_lib("provA");
    {
        std::string s = provA->exchange("{\"command\":\"status\"}");
        CHECK(jint(s, "reg_echo") == XI_CAP_REG_OK);
    }
    CHECK(cap->available("test.echo") == 1);
    CHECK(call_echo(41) == 42);

    // -----------------------------------------------------------------------
    SECTION("second instance of the SAME provider is ETAKEN but REMEMBERED (shadow)");
    auto provB = make_lib("provB");
    {
        std::string s = provB->exchange("{\"command\":\"status\"}");
        CHECK(jint(s, "reg_echo") == XI_CAP_REG_ETAKEN);   // loser return preserved
    }
    CHECK(cap->available("test.echo") == 1);               // still served by provA

    // -----------------------------------------------------------------------
    SECTION("remove the FIRST instance -> the shadow is PROMOTED, cap still resolves");
    xi::InstanceRegistry::instance().remove("provA");
    provA.reset();                                          // dtor sweep of the holder
    CHECK(cap->available("test.echo") == 1);               // B1: NOT gone — promoted to provB
    CHECK(call_echo(7) == 8);                              // and the live provider serves it

    // -----------------------------------------------------------------------
    SECTION("removing the last provider drops the name; registry balances");
    xi::InstanceRegistry::instance().remove("provB");
    provB.reset();
    CHECK(cap->available("test.echo") == 0);
    CHECK(xi::CapRegistry::instance().size() == 0);
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    FreeLibrary(dll);
    if (g_failures == 0) { std::printf("\n[test] cap provider ref-count: ALL OK\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
