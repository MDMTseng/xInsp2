//
// cap_imgcodec_test.cpp — xi.imgcodec (the first official LIB plugin) end to
// end against the REAL built DLL through the REAL capability plane (docs/
// new_gen/14): register-on-create, host-funnel calls by name, and THE DEDUP
// HEADLINE — two consumer calls with the same sealed image do ONE encode
// (encodes == 1, cache_hit on the second) and receive BYTE-IDENTICAL JPEG.
//
// Also: JPEG magic bytes, quality param forming a distinct cache key, the
// decode round trip (xi.image.decode on the bytes xi.jpeg.encode produced),
// the $probe/$v convention on both capabilities, $fault contract answers,
// stats via exchange, unregister-on-destroy, and pack-registry balance.
//
// Host role exactly like pack_pilot_test: ImagePool host_api +
// install_pack_abi + install_cap_plane; the factory runs under a pre-allocated
// owner scope and the adapter adopts it (the PM create path).
//
#include <xi/xi_abi.h>
#include <xi/xi_cap_abi.hpp>
#include <xi/xi_cabi_adapter.hpp>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_instance.hpp>
#include <xi/xi_pack_abi.hpp>
#include <xi/xi_seh.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifndef IMGCODEC_DLL_PATH
#define IMGCODEC_DLL_PATH "imgcodec/xi-imgcodec.dll"
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
static std::vector<uint8_t> pbin(const xi_pack_v1* pk, xi_pack_handle h, const char* key) {
    const void* p = nullptr; int32_t n = 0;
    if (h != XI_PACK_NULL && pk->get_bin(h, key, &p, &n) && p && n > 0)
        return std::vector<uint8_t>(static_cast<const uint8_t*>(p),
                                    static_cast<const uint8_t*>(p) + n);
    return {};
}

int main() {
    std::printf("[test] xi.imgcodec — lib plugin + dedup through the capability plane\n");
    xi::install_seh_translator();

    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    xi::install_cap_plane();
    const auto* pk  = static_cast<const xi_pack_v1*>(host.get_interface("xi.pack", 1));
    const auto* cap = static_cast<const xi_cap_v1*>(host.get_interface("xi.cap", 1));
    CHECK(pk && cap);
    if (!pk || !cap) return 1;
    const size_t frames_baseline = xi::PackRegistry::instance().live_frames();

    SECTION("load the REAL xi-imgcodec.dll; register-on-create through the PM path");
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
    CHECK(cap->available("xi.jpeg.encode") == 1);
    CHECK(cap->available("xi.image.decode") == 1);
    {
        std::string s = codec->exchange("{\"command\":\"stats\"}");
        CHECK(jint(s, "encodes") == 0);
        CHECK(jint(s, "hits") == 0);
    }

    // One sealed input image, shared by both consumer calls (the doc-14
    // motivating case: N consumers, one sealed pack, one encode).
    constexpr int W = 32, H = 24;
    std::vector<uint8_t> gray((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            gray[(size_t)y * W + x] = (uint8_t)((x * 8 + y * 3) & 0xFF);
    xi_pack_builder ib = pk->builder_new();
    pk->builder_add_image(ib, "image", W, H, 1, gray.data());
    pk->builder_add_i64(ib, "quality", 90);
    pk->builder_add_i64(ib, "$v", 1);
    xi_pack_handle req = pk->builder_seal(ib);
    CHECK(req != XI_PACK_NULL);

    SECTION("consumer 1: encode -> JPEG magic, a real encode (miss)");
    std::vector<uint8_t> jpeg1;
    {
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.jpeg.encode", req, &out) == XI_CAP_OK);
        CHECK(out != XI_PACK_NULL);
        CHECK(pstr(pk, out, "$fault").empty());
        jpeg1 = pbin(pk, out, "jpeg");
        CHECK(jpeg1.size() > 4);
        CHECK(jpeg1.size() >= 3 && jpeg1[0] == 0xFF && jpeg1[1] == 0xD8 &&
              jpeg1[2] == 0xFF);                              // JPEG SOI magic
        CHECK(pi64(pk, out, "cache_hit", -1) == 0);
        CHECK(pi64(pk, out, "encodes", -1) == 1);
        pk->release(out);
    }

    SECTION("consumer 2 (same sealed image): ONE encode total, byte-identical "
            "bytes, cache hit — THE DEDUP PROOF");
    {
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.jpeg.encode", req, &out) == XI_CAP_OK);
        std::vector<uint8_t> jpeg2 = pbin(pk, out, "jpeg");
        CHECK(pi64(pk, out, "cache_hit", -1) == 1);
        CHECK(pi64(pk, out, "encodes", -1) == 1);             // counter did NOT move
        CHECK(jpeg2 == jpeg1);                                // byte-identical
        pk->release(out);
        std::string s = codec->exchange("{\"command\":\"stats\"}");
        CHECK(jint(s, "encodes") == 1);
        CHECK(jint(s, "hits") == 1);
    }

    SECTION("params are part of the identity: a different quality re-encodes");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_image(b, "image", W, H, 1, gray.data());
        pk->builder_add_i64(b, "quality", 30);
        xi_pack_handle req30 = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.jpeg.encode", req30, &out) == XI_CAP_OK);
        CHECK(pi64(pk, out, "cache_hit", -1) == 0);
        CHECK(pi64(pk, out, "encodes", -1) == 2);
        std::vector<uint8_t> jpeg30 = pbin(pk, out, "jpeg");
        CHECK(!jpeg30.empty() && jpeg30 != jpeg1);
        pk->release(out); pk->release(req30);
    }

    SECTION("decode round trip: xi.image.decode on the produced JPEG");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_bin(b, "data", jpeg1.data(), (int32_t)jpeg1.size());
        xi_pack_handle dreq = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.image.decode", dreq, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault").empty());
        xi_pack_image img{};
        CHECK(pk->get_image(out, "image", &img) == 1);
        CHECK(img.width == W && img.height == H);
        // stb's JPEG writer emits 3-component YCbCr even for grayscale input,
        // so a gray source may legitimately round-trip as 1 OR 3 channels
        // (encoder-dependent — turbojpeg would keep TJSAMP_GRAY).
        CHECK(img.channels == 1 || img.channels == 3);
        CHECK(img.pixels != nullptr && img.length == W * H * img.channels);
        pk->release(out); pk->release(dreq);
    }

    SECTION("$probe answers the supported versions; unsupported $v is a $fault pack");
    {
        xi_pack_builder b = pk->builder_new();
        if (pk->builder_add_bool) pk->builder_add_bool(b, "$probe", 1);
        else                      pk->builder_add_i64(b, "$probe", 1);
        xi_pack_handle preq = pk->builder_seal(b);
        for (const char* name : {"xi.jpeg.encode", "xi.image.decode"}) {
            xi_pack_handle out = XI_PACK_NULL;
            CHECK(cap->call(name, preq, &out) == XI_CAP_OK);
            CHECK(pstr(pk, out, "$versions") == "1");
            pk->release(out);
        }
        pk->release(preq);

        b = pk->builder_new();
        pk->builder_add_image(b, "image", W, H, 1, gray.data());
        pk->builder_add_i64(b, "$v", 7);
        xi_pack_handle vreq = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.jpeg.encode", vreq, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "unsupported_version");
        CHECK(pstr(pk, out, "$versions") == "1");
        pk->release(out); pk->release(vreq);
    }

    SECTION("contract fail-loud: missing inputs answer $fault packs (rc stays 0)");
    {
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "quality", 50);                // no image entry
        xi_pack_handle bad = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.jpeg.encode", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "missing_input");
        pk->release(out); pk->release(bad);

        b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);                       // no data entry
        bad = pk->builder_seal(b);
        CHECK(cap->call("xi.image.decode", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "missing_input");
        pk->release(out); pk->release(bad);

        b = pk->builder_new();                                 // corrupt bytes
        const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
        pk->builder_add_bin(b, "data", junk, (int32_t)sizeof(junk));
        bad = pk->builder_seal(b);
        CHECK(cap->call("xi.image.decode", bad, &out) == XI_CAP_OK);
        CHECK(pstr(pk, out, "$fault") == "decode_failed");
        pk->release(out); pk->release(bad);
    }

    pk->release(req);

    SECTION("destroy: the lib unregisters (and the owner sweep backstops)");
    {
        xi::InstanceRegistry::instance().remove("codec");
        codec.reset();
        CHECK(cap->available("xi.jpeg.encode") == 0);
        CHECK(cap->available("xi.image.decode") == 0);
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_i64(b, "x", 1);
        xi_pack_handle in = pk->builder_seal(b);
        xi_pack_handle out = XI_PACK_NULL;
        CHECK(cap->call("xi.jpeg.encode", in, &out) == XI_CAP_EUNKNOWN);
        pk->release(in);
    }

    SECTION("pack-registry balance");
    CHECK(xi::PackRegistry::instance().live_frames() == frames_baseline);

    if (g_failures == 0) {
        std::printf("\n[test] cap_imgcodec: ALL OK\n");
        return 0;
    }
    std::fprintf(stderr, "\n[test] cap_imgcodec: %d FAILURE(S)\n", g_failures);
    return 1;
}
