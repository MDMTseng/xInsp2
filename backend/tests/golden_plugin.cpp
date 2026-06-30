//
// golden_plugin.cpp — the GOLDEN xi C-ABI plugin, pinned to the CURRENT ABI
// (XI_ABI_VERSION 9 / XI_ABI_MIN_COMPAT 6). It is the single most important ABI
// safety net (see docs/internals/adr-001-host-api-freeze.md): a minimal but REAL
// plugin that the compat test loads through the genuine plugin-load path
// (plugin_abi_compatible → CAbiInstanceAdapter) and runs process() on once,
// asserting byte-for-byte stable behaviour.
//
// It deliberately exercises the host_api surface a normal operator uses:
//   * create() stashes the host_api pointer (image_create / image_data / log).
//   * process() creates a 4x4x1 image through host->image_create, fills it with a
//     deterministic ramp via host->image_data, and returns it under key "out".
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

#include <xi/xi_abi.h>        // XI_ABI_VERSION, xi_host_api
#include <xi/xi_record.hpp>   // xi::yyjson_layout_stamp()

// The golden plugin is the v9 contract incarnate — pinned at v9 by a LITERAL,
// deliberately decoupled from the host's current XI_ABI_VERSION. As the host ABI
// advances (Phase 1 appended get_interface → v10), this plugin stays a v9 binary
// so the compat test proves the real thing: an OLD (v9 / min-compat 6) plugin
// still loads + runs unchanged against the NEWER host. Do NOT bump this to track
// the header; if a future capability needs exercising, add a SEPARATE golden for
// that version and keep this one frozen at v9. See ADR-001 / core_fix_plan.md §12.
static constexpr int kGoldenAbiVersion = 9;
static_assert(XI_ABI_VERSION >= kGoldenAbiVersion,
              "host ABI regressed below the golden's pinned v9 — the golden must "
              "remain loadable (v9 <= host version).");
static_assert(kGoldenAbiVersion >= XI_ABI_MIN_COMPAT,
              "golden's pinned v9 fell below the host min-compat floor — it would "
              "no longer load; the freeze contract is broken (ADR-001).");

namespace {

// Minimal instance state: the host_api handed to create() + one config int.
struct GoldenInstance {
    const xi_host_api* host = nullptr;
    int32_t            value = 7;   // default; round-tripped via get_def/set_def
};

} // namespace

extern "C" {

__declspec(dllexport) int xi_plugin_abi_version(void) {
    // Pinned literal — this is a v9 plugin regardless of how new the host headers
    // are (see kGoldenAbiVersion above). The loader gate accepts it on any host
    // with XI_ABI_MIN_COMPAT <= 9 <= XI_ABI_VERSION.
    return kGoldenAbiVersion;
}

__declspec(dllexport) uint32_t xi_yyjson_abi(void) {
    return xi::yyjson_layout_stamp();
}

__declspec(dllexport) void* xi_plugin_create(const xi_host_api* host, const char* /*name*/) {
    auto* inst = new GoldenInstance();
    inst->host = host;
    return inst;
}

__declspec(dllexport) void xi_plugin_destroy(void* p) {
    delete static_cast<GoldenInstance*>(p);
}

// The frozen behaviour the compat test pins: produce a 4x4 single-channel image
// whose pixel[i] == (uint8_t)(i * 16). Deterministic, host-allocated, returned
// under key "out". Exercises image_create + image_data through the real ABI.
__declspec(dllexport) void xi_plugin_process(void* p, const xi_record* /*in*/, xi_record_out* out) {
    auto* inst = static_cast<GoldenInstance*>(p);
    if (!inst || !inst->host || !inst->host->image_create) return;

    const int32_t w = 4, h = 4, ch = 1;
    xi_image_handle img = inst->host->image_create(w, h, ch);
    if (img == XI_IMAGE_NULL) return;

    uint8_t* px = inst->host->image_data ? inst->host->image_data(img) : nullptr;
    if (px) {
        const int32_t stride = inst->host->image_stride
                                 ? inst->host->image_stride(img) : (w * ch);
        for (int32_t y = 0; y < h; ++y) {
            for (int32_t x = 0; x < w; ++x) {
                const int32_t i = y * w + x;
                px[y * stride + x] = static_cast<uint8_t>((i * 16) & 0xFF);
            }
        }
    }
    xi_record_out_add_image(out, "out", img);
}

__declspec(dllexport) int xi_plugin_get_def(void* p, char* buf, int cap) {
    auto* inst = static_cast<GoldenInstance*>(p);
    int v = inst ? inst->value : 0;
    return std::snprintf(buf, (size_t)cap, "{\"value\":%d}", v);
}

__declspec(dllexport) int xi_plugin_set_def(void* p, const char* json) {
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
