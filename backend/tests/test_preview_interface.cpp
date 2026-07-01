//
// test_preview_interface.cpp — Phase 2 proof: the carved xi.preview@1 interface
// compresses byte-for-byte IDENTICALLY to the legacy compress_image field.
//
// core_fix_plan.md §12 Phase 2 carves the JPEG-encode capability out of the v9
// monolith's compress_image field into a frozen, segregated interface resolved
// through host->get_interface("xi.preview", 1). This test pins the contract that
// makes the carve safe: a caller reaching the capability via the NEW door gets
// exactly the same bytes as one calling the OLD field — both funnel through the
// host's single content-addressed encoder (compress_image_impl), so neither path
// is privileged and an old plugin can keep using the field forever.
//
// It also exercises the SDK wrapper xi::Plugin::compress(), which resolves the
// interface once with a legacy-field fallback, proving:
//   * a v10 host with xi.preview published  -> wrapper uses the interface;
//   * a (simulated) pre-v10 host (get_interface null) -> wrapper falls back to
//     compress_image; both yield identical bytes.
//
#include <xi/xi_abi.h>
#include <xi/xi_abi.hpp>          // xi::Plugin (SDK compress wrapper)
#include <xi/xi_image_pool.hpp>   // xi::ImagePool::make_host_api
#include <xi/xi_compress_sink.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// A deterministic stand-in for the real JPEG encoder. The test only needs both
// capability paths to hit the SAME installed sink, so a simple reversible
// transform suffices — and its explicit -needed contract exercises the
// resize-and-retry branch the SDK wrapper relies on.
static int32_t fake_encode(const void* px, int32_t w, int32_t h, int32_t ch,
                           int32_t q, void* out, int32_t cap) {
    const int32_t need = w * h * ch + 1;            // +1 byte: quality header
    if (!out || cap < need) return -need;           // caller resizes + retries
    auto* o = static_cast<uint8_t*>(out);
    const auto* in = static_cast<const uint8_t*>(px);
    o[0] = static_cast<uint8_t>(q);
    for (int32_t i = 0; i < w * h * ch; ++i)
        o[i + 1] = static_cast<uint8_t>(in[i] ^ 0x5A);
    return need;
}

// Compress through a raw function-pointer path (legacy field or interface entry),
// honoring the -needed resize protocol; returns the produced JPEG bytes.
template <class Fn>
static std::vector<uint8_t> run_compress(Fn fn, const std::vector<uint8_t>& px,
                                         int w, int h, int ch, int q) {
    std::vector<uint8_t> out(4);                    // deliberately too small first
    int n = fn(px.data(), w, h, ch, q, out.data(), (int)out.size());
    if (n < 0) {                                    // resize to needed + retry
        out.resize((size_t)(-n));
        n = fn(px.data(), w, h, ch, q, out.data(), (int)out.size());
    }
    if (n <= 0) return {};
    out.resize((size_t)n);
    return out;
}

int main() {
    std::printf("[test] xi.preview@1 vs legacy compress_image — identical bytes\n");

    // The interface is the v10 carve; pin so a careless ABI edit fails the build.
    static_assert(XI_ABI_VERSION >= 10, "xi.preview test assumes the v10 door");

    xi::compress_sink() = &fake_encode;             // install the host encoder
    xi_host_api host = xi::ImagePool::make_host_api();

    // A small image with a deterministic ramp.
    const int w = 4, h = 4, ch = 1;
    std::vector<uint8_t> px((size_t)(w * h * ch));
    for (size_t i = 0; i < px.size(); ++i) px[i] = (uint8_t)((i * 16) & 0xFF);
    const int quality = 85;

    // (1) The v10 door publishes xi.preview@1, and it carries the same single
    // compress entry.
    CHECK(host.get_interface != nullptr);
    const auto* pv = static_cast<const xi_preview_v1*>(
        host.get_interface("xi.preview", 1));
    CHECK(pv != nullptr);
    CHECK(pv && pv->compress != nullptr);

    // (2) Core proof: legacy field path == interface path, byte for byte.
    std::vector<uint8_t> legacy = run_compress(host.compress_image, px, w, h, ch, quality);
    std::vector<uint8_t> iface  = pv ? run_compress(pv->compress, px, w, h, ch, quality)
                                     : std::vector<uint8_t>{};
    CHECK(!legacy.empty());
    CHECK(legacy == iface);

    // (3) SDK wrapper on a v10 host resolves the interface and matches.
    {
        xi::Plugin plug(&host, "preview_test");
        std::vector<uint8_t> via_sdk = run_compress(
            [&](const void* p, int32_t ww, int32_t hh, int32_t cc, int32_t qq,
                void* o, int32_t cap) { return plug.compress(p, ww, hh, cc, qq, o, cap); },
            px, w, h, ch, quality);
        CHECK(via_sdk == legacy);
    }

    // (4) SDK wrapper fallback: simulate a pre-v10 host (no get_interface door)
    // that still has the legacy compress_image field. The wrapper must fall back
    // and produce identical bytes.
    {
        xi_host_api old_host = host;
        old_host.get_interface = nullptr;           // pre-v10 host
        xi::Plugin plug(&old_host, "preview_legacy");
        std::vector<uint8_t> via_fallback = run_compress(
            [&](const void* p, int32_t ww, int32_t hh, int32_t cc, int32_t qq,
                void* o, int32_t cap) { return plug.compress(p, ww, hh, cc, qq, o, cap); },
            px, w, h, ch, quality);
        CHECK(via_fallback == legacy);
    }

    // (5) Unknown / wrong-version queries return null (a caller then falls back).
    CHECK(host.get_interface("xi.preview", 2) == nullptr);
    CHECK(host.get_interface("xi.nope", 1) == nullptr);
    CHECK(host.get_interface(nullptr, 1) == nullptr);

    // (6) xi.legacy@9 was RETIRED in Phase 4 — the door no longer answers it.
    // The compress capability lives on via xi.preview@1 (proven above) and the
    // struct field directly.
    CHECK(host.get_interface("xi.legacy", 9) == nullptr);

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
