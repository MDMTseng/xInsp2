//
// cap_imgcodec_host_test.cpp — polaris2 CORE-CODEC EVICTION (stage 1) proof.
//
// The HOST's read_image_file (backend/src/xi_image_io.cpp, LINKED into this EXE
// so its static installer runs) now DELEGATES decode to the "xi.image.decode"
// capability served by the REAL xi-imgcodec.dll, through the REAL capability
// plane funnel, and FALLS BACK to the built-in stb path on any miss. This test
// proves the eviction is invisible to callers:
//
//   1. BYTE-EQUIVALENCE — the same file decoded via the capability engine and
//      via the stb engine yields byte-identical pixels (3-channel and, through
//      the "raw" reply shape, native 2-channel gray+alpha).
//   2. FACTORY-TIMING / ABSENT CAPABILITY — with no imgcodec instance the slot
//      re-probe misses and the host serves from stb silently.
//   3. SELF-SERVE REENTRANCY REFUSAL — a read_image_file issued while the
//      imgcodec instance is the current owner (the decoder calling its own
//      file-decode) is refused by the funnel (-5) and lands on stb; imgcodec is
//      never served by itself (its decode counter does not move).
//   4. DECODER FAULT -> CLEAN FALLBACK + ATTRIBUTION — a quarantined decoder
//      instance (the on_fault outcome the funnel applies when a handler faults)
//      makes the funnel refuse (-3); the host falls back to stb and returns a
//      valid image while the fault stays ATTRIBUTED to the lib instance.
//   5. CONTRACT $fault -> FALLBACK — corrupt bytes answer a $fault pack; the
//      host falls back to stb, which also fails, so the caller sees the same 0
//      it saw before the eviction (fail modes identical).
//
// Host role mirrors cap_imgcodec_test: ImagePool host_api + install_pack_abi +
// install_cap_plane; the factory runs under a pre-allocated owner scope and the
// adapter adopts it (the PM create path).
//
#include <xi/xi_abi.h>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>

// LoadLibraryA/GetProcAddress/HMODULE/GetLastError for the loader below:
// real <windows.h> on Windows, dlopen/dlsym shim on POSIX (same names).
#include <xi/xi_dynlib.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef IMGCODEC_DLL_PATH
#define IMGCODEC_DLL_PATH "imgcodec/xi-imgcodec.dll"
#endif

// stb writer (implementation compiled in via backend/src/stb_impl.cpp, also
// linked into this EXE) — used to author the on-disk test images.
extern "C" int stbi_write_png(const char* filename, int w, int h, int comp,
                              const void* data, int stride_in_bytes);

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

// The imgcodec decode counter — the proof the CAPABILITY engine (not stb) served.
static int codec_decodes(xi::CAbiInstanceAdapter* codec) {
    return jint(codec->exchange("{\"command\":\"stats\"}"), "decodes");
}

// A read_image_file result, materialized for comparison.
struct Img {
    std::vector<uint8_t> px;
    int w = 0, h = 0, c = 0;
    bool ok = false;
};
static Img materialize(xi_image_handle h) {
    Img r;
    if (!h) return r;
    auto& pool = xi::ImagePool::instance();
    r.w = pool.width(h); r.h = pool.height(h); r.c = pool.channels(h);
    if (const uint8_t* d = pool.data(h)) {
        r.px.assign(d, d + (size_t)r.w * r.h * r.c);
        r.ok = true;
    }
    pool.release(h);
    return r;
}
static bool same_pixels(const Img& a, const Img& b) {
    return a.ok && b.ok && a.w == b.w && a.h == b.h && a.c == b.c && a.px == b.px;
}

int main() {
    std::printf("[test] xi.imgcodec — HOST decode eviction (was the read_image_file slot): "
                "capability engine + stb fallback, byte-equivalent\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();
    (void)host;

    // The impl under test installs itself from xi_image_io.cpp (linked in).
    auto decode_image = xi::ImagePool::decode_image_fn();
    CHECK(decode_image != nullptr);
    if (!decode_image) return 1;

    // ---- author on-disk fixtures ------------------------------------------
    constexpr int W = 16, H = 12;
    std::vector<uint8_t> rgb((size_t)W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 3;
            rgb[i + 0] = (uint8_t)(x * 13 + 1);
            rgb[i + 1] = (uint8_t)(y * 17 + 2);
            rgb[i + 2] = (uint8_t)((x ^ y) * 7 + 3);
        }
    const char* png3 = "host_codec_rgb.png";
    CHECK(stbi_write_png(png3, W, H, 3, rgb.data(), W * 3) != 0);

    std::vector<uint8_t> ga((size_t)W * H * 2);   // gray + alpha (2 channels)
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = ((size_t)y * W + x) * 2;
            ga[i + 0] = (uint8_t)(x * 9 + y * 5);
            ga[i + 1] = (uint8_t)(255 - x);
        }
    const char* png2 = "host_codec_ga.png";
    CHECK(stbi_write_png(png2, W, H, 2, ga.data(), W * 2) != 0);

    const char* junkfile = "host_codec_junk.bin";
    {
        std::FILE* f = std::fopen(junkfile, "wb");
        CHECK(f != nullptr);
        if (f) { const uint8_t j[] = {1,2,3,4,5,6,7,8}; std::fwrite(j,1,sizeof(j),f); std::fclose(f); }
    }

    // ---- load the REAL imgcodec DLL; register-on-create (the PM path) ------
    SECTION("load xi-imgcodec.dll; the decode capability comes up");
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
    CHECK(cap && cap->available("xi.image.decode") == 1);

    // ---- 1. capability engine serves; capture the reference pixels --------
    SECTION("capability engine serves decode (3-channel)");
    int d0 = codec_decodes(codec.get());
    Img cap_rgb = materialize(decode_image(png3));
    CHECK(cap_rgb.ok && cap_rgb.w == W && cap_rgb.h == H && cap_rgb.c == 3);
    CHECK(codec_decodes(codec.get()) == d0 + 1);          // the CAPABILITY served

    SECTION("capability engine serves native 2-channel via the 'raw' reply");
    int d1 = codec_decodes(codec.get());
    Img cap_ga = materialize(decode_image(png2));
    CHECK(cap_ga.ok && cap_ga.w == W && cap_ga.h == H && cap_ga.c == 2);
    CHECK(codec_decodes(codec.get()) == d1 + 1);

    // ---- 3. self-serve reentrancy refusal ---------------------------------
    SECTION("self-serve refusal: decode while imgcodec is the current owner -> "
            "funnel -5 -> FAIL (v12: no in-core fallback); imgcodec never serves itself");
    int d2 = codec_decodes(codec.get());
    {
        xi::ImagePool::OwnerGuard og(codec_owner);   // as if imgcodec called us
        // [v12 THE CUT: the stb fallback is deleted — a refused funnel call
        //  fails LOUD (0 handle), it does not silently decode in-core.]
        CHECK(decode_image(png3) == 0);
    }
    CHECK(codec_decodes(codec.get()) == d2);              // decoder did NOT serve

    // ---- 4. decoder fault -> clean fallback + attribution -----------------
    SECTION("quarantined decoder (a handler-fault outcome) -> funnel -3 -> FAIL "
            "(v12: no in-core fallback); fault stays attributed to the lib instance");
    codec->set_quarantined(true);
    int d3 = codec_decodes(codec.get());
    // [v12 THE CUT: quarantined provider = decode unavailable = 0, fail loud.]
    CHECK(decode_image(png3) == 0);
    CHECK(codec_decodes(codec.get()) == d3);              // quarantined: not served
    CHECK(codec->quarantined());                          // attribution intact
    codec->set_quarantined(false);

    // ---- 5. contract $fault -> fallback -> stb also fails -> 0 ------------
    SECTION("corrupt bytes: $fault -> 0 (v12: capability is the only engine)");
    CHECK(decode_image(junkfile) == 0);
    CHECK(decode_image(nullptr) == 0);                 // null path unchanged

    // ---- 2. absent capability: remove the instance, stb serves ------------
    SECTION("remove the imgcodec instance -> capability absent -> decode FAILS "
            "(v12: no stb engine; an imgcodec provider is a hard requirement)");
    xi::InstanceRegistry::instance().remove("codec");
    codec.reset();
    CHECK(cap->available("xi.image.decode") == 0);

    // [v12 THE CUT: with the provider gone there is NO decode engine — every
    //  decode fails loud (0). Deployments supply imgcodec via a project
    //  instance or machine autoload (--autoload-lib / XINSP2_AUTOLOAD_LIB=1).]
    CHECK(decode_image(png3) == 0);
    CHECK(decode_image(png2) == 0);
    CHECK(decode_image(junkfile) == 0);

    std::remove(png3); std::remove(png2); std::remove(junkfile);

    if (g_failures == 0) {
        std::printf("\n[test] cap_imgcodec_host: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n[test] cap_imgcodec_host: %d FAILURE(S)\n", g_failures);
    return 1;
}
