//
// golden_plugin.cpp — the GOLDEN xi C-ABI plugin, pinned to the CURRENT ABI
// (XI_ABI_VERSION 11 / XI_ABI_MIN_COMPAT 11). It is the single most important ABI
// safety net (see docs/internals/adr-001-host-api-freeze.md): a minimal but REAL
// plugin that the compat test loads through the genuine plugin-load path
// (plugin_abi_compatible → CAbiInstanceAdapter) and runs process() on once,
// asserting byte-for-byte stable behaviour.
//
// It deliberately exercises the host_api surface a normal operator uses:
//   * create() stashes the host_api pointer and resolves the xi.pack@1 host door
//     (host->get_interface("xi.pack",1)).
//   * the xi.pack@1 data-plane door (xi_pack_proc_v1, published via
//     xi_plugin_get_interface) builds a 4x4x1 image with a deterministic ramp
//     through the host pack builder and returns it sealed under key "out".
//   * get_def / set_def round-trip a single integer config field.
//
// Built from SOURCE against the live headers (not a committed binary blob), so it
// always reflects the current ABI — but its EXPORTED behaviour is frozen by the
// test's assertions. If a future ABI change alters how an old-but-valid plugin
// loads or how process() observes the host, THIS test breaks first.
//
// Raw C exports (no XI_PLUGIN_IMPL) so the test can resolve them with
// GetProcAddress exactly as the loader does, and so the fixture has no dependency
// on the higher-level SDK macros.
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>

#include <xi/xi_abi.h>        // XI_ABI_VERSION, xi_host_api, xi.pack@1 types

// The golden plugin is the CURRENT contract incarnate. Phase 4 retired the
// monolith with an authorized major break (shm_* removed, xi.legacy retired,
// min-compat raised to 11), which by design invalidated every pre-v11 plugin —
// so the OLD "v9 plugin loads on a newer host" scenario no longer exists (that
// break is now PROVEN by test_golden_plugin.cpp's stale-plugin-REFUSED
// assertion). This golden is therefore re-pinned to v11: it proves a CURRENT
// plugin still loads + runs. It tracks XI_ABI_MIN_COMPAT so it always sits
// exactly at the floor — the oldest still-loadable version. See ADR-001 /
// core_fix_plan.md §12 Phase 4.
static constexpr int kGoldenAbiVersion = XI_ABI_MIN_COMPAT;   // == 11 at v11
static_assert(XI_ABI_VERSION >= kGoldenAbiVersion,
              "host ABI regressed below the golden's pinned version — the golden "
              "must remain loadable (kGoldenAbiVersion <= host version).");
static_assert(kGoldenAbiVersion >= XI_ABI_MIN_COMPAT,
              "golden's pinned version fell below the host min-compat floor — it "
              "would no longer load; the freeze contract is broken (ADR-001).");

namespace {

// Minimal instance state: the host_api handed to create() + the resolved
// xi.pack@1 host door (data plane) + one config int.
struct GoldenInstance {
    const xi_host_api* host = nullptr;
    const xi_pack_v1*  pack = nullptr;   // resolved via host->get_interface("xi.pack",1)
    int32_t            value = 7;   // default; round-tripped via get_def/set_def
};

// The frozen behaviour the compat test pins: produce a 4x4 single-channel image
// whose pixel[i] == (uint8_t)(i * 16). Deterministic, built through the host's
// xi.pack@1 builder, returned under key "out". The old Record path built the
// same ramp with image_stride; for w=4,c=1 the stride is 4, so a row-major
// 4x4 buffer with gray[i] == i*16 is byte-for-byte identical.
static xi_pack_handle golden_pack_process(void* p, xi_pack_handle /*input*/) {
    auto* inst = static_cast<GoldenInstance*>(p);
    if (!inst || !inst->pack) return XI_PACK_NULL;

    std::vector<uint8_t> gray(16);
    for (int i = 0; i < 16; ++i) gray[i] = static_cast<uint8_t>((i * 16) & 0xFF);

    xi_pack_builder b = inst->pack->builder_new();
    inst->pack->builder_add_image(b, "out", 4, 4, 1, gray.data());
    return inst->pack->builder_seal(b);
}

} // namespace

extern "C" {

XI_EXPORT int xi_plugin_abi_version(void) {
    // A current (v11) plugin. The loader gate accepts it on any host with
    // XI_ABI_MIN_COMPAT <= kGoldenAbiVersion <= XI_ABI_VERSION.
    return kGoldenAbiVersion;
}

// The sole plugin data plane: the xi.pack@1 door (xi_pack_proc_v1). The host
// probes this to learn the golden speaks packs and drives it via run_pack_door.
XI_EXPORT const void* xi_plugin_get_interface(const char* id, uint32_t version) {
    if (id && version == 1u && std::strcmp(id, "xi.pack") == 0) {
        static const xi_pack_proc_v1 iface = { &golden_pack_process };
        return &iface;
    }
    return nullptr;
}

XI_EXPORT void* xi_plugin_create(const xi_host_api* host, const char* /*name*/) {
    auto* inst = new GoldenInstance();
    inst->host = host;
    if (host && host->get_interface)
        inst->pack = static_cast<const xi_pack_v1*>(host->get_interface("xi.pack", 1));
    return inst;
}

XI_EXPORT void xi_plugin_destroy(void* p) {
    delete static_cast<GoldenInstance*>(p);
}

XI_EXPORT int xi_plugin_get_def(void* p, char* buf, int cap) {
    auto* inst = static_cast<GoldenInstance*>(p);
    int v = inst ? inst->value : 0;
    return std::snprintf(buf, (size_t)cap, "{\"value\":%d}", v);
}

XI_EXPORT int xi_plugin_set_def(void* p, const char* json) {
    auto* inst = static_cast<GoldenInstance*>(p);
    if (!inst || !json) return 1;
    // Tiny hand parse: find "value": and read the int. Avoids pulling yyjson into
    // the fixture; the golden test only needs the round-trip to be observable.
    const char* k = std::strstr(json, "\"value\"");
    if (k) {
        const char* c = std::strchr(k, ':');
        if (c) inst->value = std::atoi(c + 1);
    }
    return 0;
}

} // extern "C"
