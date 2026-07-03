//
// cap_jpeg_encode_host_test.cpp — polaris2 CORE-CODEC EVICTION (stage 2) proof.
//
// The HOST's preview JPEG encode (backend compress_sink) now DELEGATES to the
// "xi.jpeg.encode" capability served by the REAL xi-imgcodec.dll, through the
// REAL capability plane funnel, and FALLS BACK to the built-in in-core encoder
// (xi/xi_jpeg.hpp, xi::encode_jpeg) on any miss. This exercises the delegation
// funnel directly (backend/include/xi/xi_jpeg_cap.hpp, the same helper the
// compress_sink lambda calls) and proves the eviction is invisible to callers:
//
//   1. CAPABILITY ENGINE SERVES + BYTE-EQUIVALENCE — encode via the capability
//      yields a decodable JPEG whose bytes EQUAL the in-core xi::encode_jpeg
//      output for the same image + quality (both stb in the default build; both
//      turbojpeg under XINSP2_HAS_TURBOJPEG — same tjCompress2 params). The
//      imgcodec encode counter moves: the CAPABILITY served, not the fallback.
//   2. DEDUP DEFERS TO IMGCODEC — a repeat of the same image+quality is a cache
//      HIT inside imgcodec (its encode counter does NOT move again), so the host
//      keeps no second cache on this path (no double-caching).
//   3. QUALITY PLUMBING — a different quality is a distinct encode, byte-equal to
//      the in-core encoder at that quality.
//   4. SELF-SERVE REENTRANCY REFUSAL — an encode issued while the imgcodec
//      instance is the current owner is refused by the funnel (-5): the helper
//      returns false (caller falls back to in-core); imgcodec never encodes for
//      itself (its counter does not move).
//   5. ENCODER FAULT -> CLEAN FALLBACK + ATTRIBUTION — a quarantined encoder (a
//      handler-fault outcome) makes the funnel refuse (-3); the helper returns
//      false and the fault stays ATTRIBUTED to the lib instance.
//   6. ABSENT CAPABILITY -> IN-CORE FALLBACK — with no imgcodec instance the slot
//      re-probe misses; the helper returns false and the in-core encoder still
//      produces a valid, decodable JPEG (pre-eviction behaviour).
//
// Host role mirrors cap_imgcodec_host_test: ImagePool host_api + install_pack_abi
// + install_cap_plane; the factory runs under a pre-allocated owner scope.
//
#include <xi/xi_abi.h>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_image.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_jpeg.hpp>       // in-core reference encoder (the fallback)
#include <xi/xi_jpeg_cap.hpp>   // encode_via_capability — the delegation funnel
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef IMGCODEC_DLL_PATH
#define IMGCODEC_DLL_PATH "imgcodec/xi-imgcodec.dll"
#endif

// stb decode (implementation compiled in via backend/src/stb_impl.cpp, linked
// into this EXE) — used only to prove the encoded bytes are a decodable JPEG.
extern "C" unsigned char* stbi_load_from_memory(
    const unsigned char* buffer, int len, int* x, int* y,
    int* channels_in_file, int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

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
// The imgcodec encode counter — the proof the CAPABILITY engine encoded.
static int codec_encodes(xi::CAbiInstanceAdapter* codec) {
    return jint(codec->exchange("{\"command\":\"stats\"}"), "encodes");
}

// A JPEG is decodable back to the input dims (content is lossy, dims are not).
static bool decodable_jpeg(const std::vector<uint8_t>& jpeg, int w, int h) {
    int dw = 0, dh = 0, dc = 0;
    unsigned char* px = stbi_load_from_memory(jpeg.data(), (int)jpeg.size(),
                                              &dw, &dh, &dc, 0);
    bool ok = px && dw == w && dh == h;
    if (px) stbi_image_free(px);
    return ok;
}

int main() {
    std::printf("[test] xi.imgcodec — HOST compress_sink encode eviction: "
                "capability engine + in-core fallback, byte-equivalent\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();
    (void)host;

    // ---- author an in-memory RGB image (the preview-frame stand-in) --------
    constexpr int W = 24, H = 18, C = 3;
    std::vector<uint8_t> pix((size_t)W * H * C);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * C;
            pix[i + 0] = (uint8_t)(x * 11 + 1);
            pix[i + 1] = (uint8_t)(y * 13 + 2);
            pix[i + 2] = (uint8_t)((x ^ y) * 5 + 3);
        }
    xi::Image img = xi::Image::view(W, H, C, pix.data());

    // The in-core reference encoder (the v12-fallback path) — same engine the
    // capability uses, so the bytes must match exactly.
    std::vector<uint8_t> core85;
    CHECK(xi::encode_jpeg(img, 85, core85) && !core85.empty());

    // ---- before any imgcodec instance: capability absent -> helper false ---
    SECTION("no imgcodec instance -> encode_via_capability false -> in-core serves");
    {
        std::vector<uint8_t> j;
        CHECK(xi::encode_via_capability(img, 85, j) == false);
        CHECK(j.empty());
    }

    // ---- load the REAL imgcodec DLL; register-on-create (the PM path) ------
    SECTION("load xi-imgcodec.dll; the encode capability comes up");
    HMODULE dll = LoadLibraryA(IMGCODEC_DLL_PATH);
    if (!dll) {
        std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n",
                     IMGCODEC_DLL_PATH, GetLastError());
        return 1;
    }
    auto create = reinterpret_cast<xi::PluginInfo::CFactoryFn>(
        GetProcAddress(dll, "xi_plugin_create"));
    CHECK(create != nullptr);
    if (!create) return 1;

    std::shared_ptr<xi::CAbiInstanceAdapter> codec;
    {
        xi::ImagePoolOwnerScope scope;
        void* raw = scope.run_factory([&] { return create(&host, "codec"); });
        CHECK(raw != nullptr);
        if (!raw) return 1;
        codec = std::make_shared<xi::CAbiInstanceAdapter>(
            "codec", "imgcodec", dll, raw, /*reentrant=*/true, /*max_conc=*/0);
        codec->adopt_owner_id(scope.release());
        xi::InstanceRegistry::instance().add(codec);
    }
    const auto codec_owner = codec->owner_id();

    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    CHECK(cap && cap->available("xi.jpeg.encode") == 1);

    // ---- 1. capability engine serves; bytes EQUAL the in-core encoder ------
    SECTION("capability engine serves encode (byte-equivalent to in-core)");
    int e0 = codec_encodes(codec.get());
    std::vector<uint8_t> cap85;
    CHECK(xi::encode_via_capability(img, 85, cap85) && !cap85.empty());
    CHECK(codec_encodes(codec.get()) == e0 + 1);   // the CAPABILITY encoded
    CHECK(decodable_jpeg(cap85, W, H));
    CHECK(cap85 == core85);                          // BYTE-EQUIVALENCE (q=85)

    // ---- 2. dedup defers to imgcodec: repeat is a cache HIT there ----------
    SECTION("repeat encode is imgcodec cache HIT -> no second encode, no host cache");
    int e1 = codec_encodes(codec.get());
    std::vector<uint8_t> cap85b;
    CHECK(xi::encode_via_capability(img, 85, cap85b) && cap85b == cap85);
    CHECK(codec_encodes(codec.get()) == e1);         // imgcodec did NOT re-encode

    // ---- 3. quality plumbing ----------------------------------------------
    SECTION("quality param plumbs through to the capability (distinct encode)");
    std::vector<uint8_t> core50;
    CHECK(xi::encode_jpeg(img, 50, core50) && !core50.empty());
    std::vector<uint8_t> cap50;
    CHECK(xi::encode_via_capability(img, 50, cap50) && !cap50.empty());
    CHECK(cap50 == core50);                          // byte-equivalent at q=50
    CHECK(cap50 != cap85);                           // quality actually varied

    // ---- 4. self-serve reentrancy refusal ---------------------------------
    SECTION("self-serve refusal: encode while imgcodec is the current owner -> "
            "funnel -5 -> helper false; imgcodec never encodes for itself");
    int e2 = codec_encodes(codec.get());
    {
        xi::ImagePool::OwnerGuard og(codec_owner);   // as if imgcodec called us
        std::vector<uint8_t> j;
        CHECK(xi::encode_via_capability(img, 85, j) == false);
        CHECK(j.empty());
    }
    CHECK(codec_encodes(codec.get()) == e2);         // encoder did NOT serve itself

    // ---- 5. encoder fault -> clean fallback + attribution -----------------
    SECTION("quarantined encoder (a handler-fault outcome) -> funnel -3 -> helper "
            "false; fault stays attributed to the lib instance");
    codec->set_quarantined(true);
    int e3 = codec_encodes(codec.get());
    {
        std::vector<uint8_t> j;
        CHECK(xi::encode_via_capability(img, 85, j) == false);
    }
    CHECK(codec_encodes(codec.get()) == e3);         // quarantined: not served
    CHECK(codec->quarantined());                     // attribution intact
    codec->set_quarantined(false);

    // ---- 6. absent capability: remove the instance, in-core serves --------
    SECTION("remove the imgcodec instance -> capability absent -> in-core encoder, "
            "byte-identical to the capability engine's output");
    xi::InstanceRegistry::instance().remove("codec");
    codec.reset();
    CHECK(cap->available("xi.jpeg.encode") == 0);
    {
        std::vector<uint8_t> j;
        CHECK(xi::encode_via_capability(img, 85, j) == false);
    }
    std::vector<uint8_t> core85b;
    CHECK(xi::encode_jpeg(img, 85, core85b) && core85b == core85);   // stable
    CHECK(decodable_jpeg(core85b, W, H));

    if (g_failures == 0) {
        std::printf("\n[test] cap_jpeg_encode_host: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n[test] cap_jpeg_encode_host: %d FAILURE(S)\n", g_failures);
    return 1;
}
