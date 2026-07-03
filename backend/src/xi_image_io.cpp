// Out-of-line implementation of host_api->read_image_file.
// Lives in its own TU so xi_image_pool.hpp doesn't need to drag
// stb_image.h into every consumer; the STB_IMAGE_IMPLEMENTATION
// macro is defined once in stb_impl.cpp.
//
// Static initialiser installs the function pointer into ImagePool
// so make_host_api hands it out. Tests that don't link this TU
// leave the slot null and read_image_file is unavailable to plugins
// they spawn — fine for in-process unit tests.
//
// polaris2 CORE-CODEC EVICTION, stage 1 of 2 (v11-compatible):
// "jpeg移出core，使用lib plugin處理" — the decode ENGINE leaves the core
// while the frozen ABI slot stays. The implementation now DELEGATES to the
// "xi.image.decode" capability (the xi.imgcodec lib plugin) whenever one is
// registered, through the SAME forwarding funnel every capability consumer
// uses — quarantine fail-fast, per-thread reentrancy refusal, SEH fault
// attribution to the lib instance (xi_cap_abi.hpp's f_cap_call). On
// capability absent / funnel refusal / handler fault / contract $fault it
// falls back to the built-in stb path, byte-for-byte the pre-eviction
// behaviour. The slot, its signature, and its fail modes are IDENTICAL
// either way (0 on null path / unreadable file / undecodable bytes) —
// callers cannot tell, except the engine-transition log/status line
// (deployment hygiene: which engine is actually serving?).
//
// Availability is re-checked on EVERY call: read_image_file legally runs
// inside plugin factories, i.e. possibly BEFORE imgcodec's own factory has
// registered the capability (absent-then-present), and the capability can
// vanish again on unload. And the funnel's reentrancy guard (-5) guarantees
// imgcodec is never served by itself — that refusal lands on the stb
// fallback like every other miss.
//
// At v12 this slot AND the stb fallback are deleted; the capability becomes
// the only engine (docs/new_gen/14 roster / doc 10 cut contents).

#include <xi/xi_image_pool.hpp>
#include <stb_image.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

namespace xi {

namespace {

// ---- engine observability --------------------------------------------------
// Logged via stderr + the status registry on every ENGINE TRANSITION (first
// serving call included), not per call — steady state stays quiet, and the
// last status line always names the engine currently serving.
enum : int { kEngineStb = 0, kEngineCap = 1 };

void note_engine(int engine) {
    static std::atomic<int> last{-1};
    if (last.exchange(engine, std::memory_order_relaxed) == engine) return;
    const char* text = (engine == kEngineCap)
        ? "engine=capability (xi.image.decode, lib plugin)"
        : "engine=builtin (stb)";
    std::fprintf(stderr, "[xinsp2] read_image_file: %s\n", text);
    if (auto fn = xi::status_sink()) fn("read_image_file", text);
}

// ---- the built-in engine (the pre-eviction path, bytes unchanged) -----------
xi_image_handle read_stb(const char* path) {
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load(path, &w, &h, &ch, 0);
    if (!px) return 0;
    auto& pool = ImagePool::instance();
    xi_image_handle handle = pool.create(w, h, ch);
    if (!handle) { stbi_image_free(px); return 0; }
    if (uint8_t* dst = pool.data(handle)) {
        std::memcpy(dst, px, (size_t)w * h * ch);
    }
    stbi_image_free(px);
    return handle;
}

// ---- the capability engine ---------------------------------------------------
// Try to serve `path` through the registered "xi.image.decode" capability.
// Returns false when the request was NOT served — capability plane not
// installed, capability unregistered, file unreadable, funnel refusal
// (quarantine -3, reentrancy -5: the decoder's own instance is up-stack),
// handler crash -2 (already charged to the LIB instance by the funnel),
// contract $fault, or a malformed reply — and the caller falls back to stb.
bool read_via_capability(const char* path, xi_image_handle* out_h) {
    *out_h = 0;
    // The plane rides the same published slots get_interface serves — null on
    // a host without the pack/cap planes (headless unit tests): fallback.
    const auto* cap = static_cast<const xi_cap_v1*>(
        ImagePool::cap_iface_slot().load(std::memory_order_acquire));
    const auto* pk = static_cast<const xi_pack_v1*>(
        ImagePool::pack_iface_slot().load(std::memory_order_acquire));
    if (!cap || !pk || !cap->available || !cap->call) return false;
    if (!cap->available("xi.image.decode")) return false;   // re-probed per call

    // The request carries the FILE BYTES (bin "data") — the capability decodes
    // memory; the host keeps file I/O. Plain fopen, matching stbi_load's own
    // file access (no STBI_WINDOWS_UTF8 in stb_impl.cpp), so an unreadable
    // path misses here exactly where stb would fail — fail modes unchanged.
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<unsigned char> bytes;
    {
        unsigned char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(f);
    if (bytes.empty()) return false;

    xi_pack_builder b = pk->builder_new();
    pk->builder_add_bin(b, "data", bytes.data(), (int32_t)bytes.size());
    // "raw": permit a 2-channel (gray+alpha) reply as bin "pixels" + i64
    // w/h/c — pack image entries cannot tag 2 channels, and byte-equivalence
    // with the stb path requires the file's TRUE channel count.
    pk->builder_add_i64(b, "raw", 1);
    xi_pack_handle in = pk->builder_seal(b);
    if (in == XI_PACK_NULL) return false;

    xi_pack_handle out = XI_PACK_NULL;
    int32_t rc = cap->call("xi.image.decode", in, &out);
    pk->release(in);
    if (rc != XI_CAP_OK || out == XI_PACK_NULL) return false;

    // A contract $fault (undecodable / rejected request) -> fallback; for
    // genuinely corrupt bytes stb fails too, so the caller sees the same 0.
    {
        const char* fp = nullptr; int32_t fl = 0;
        if (pk->get_str(out, "$fault", &fp, &fl)) {
            pk->release(out);
            return false;
        }
    }

    int32_t w = 0, h = 0, c = 0;
    const void* px = nullptr;
    size_t n = 0;
    xi_pack_image img{};
    if (pk->get_image(out, "image", &img) && img.pixels) {
        w = img.width; h = img.height; c = img.channels;
        px = img.pixels; n = (size_t)img.length;
    } else {
        // The raw 2-channel form: bin "pixels" + dims.
        const void* bin = nullptr; int32_t bl = 0;
        int64_t wv = 0, hv = 0, cv = 0;
        if (pk->get_bin(out, "pixels", &bin, &bl) && bin && bl > 0 &&
            pk->get_i64(out, "w", &wv) && pk->get_i64(out, "h", &hv) &&
            pk->get_i64(out, "c", &cv)) {
            w = (int32_t)wv; h = (int32_t)hv; c = (int32_t)cv;
            px = bin; n = (size_t)bl;
        }
    }
    if (!px || w <= 0 || h <= 0 || c <= 0 ||
        n != (size_t)w * (size_t)h * (size_t)c) {
        pk->release(out);
        return false;
    }

    auto& pool = ImagePool::instance();
    xi_image_handle handle = pool.create(w, h, c);
    if (!handle) { pk->release(out); return false; }
    if (uint8_t* dst = pool.data(handle)) std::memcpy(dst, px, n);
    pk->release(out);
    *out_h = handle;
    return true;
}

} // namespace

static xi_image_handle read_image_file_impl(const char* path) {
    if (!path) return 0;
    xi_image_handle h = 0;
    if (read_via_capability(path, &h)) {
        note_engine(kEngineCap);
        return h;
    }
    h = read_stb(path);
    if (h) note_engine(kEngineStb);   // an engine is noted only when it SERVED
    return h;
}

namespace {
struct InstallReadImageFile {
    InstallReadImageFile() {
        ImagePool::install_read_image_file(&read_image_file_impl);
    }
};
static InstallReadImageFile s_install;
}

} // namespace xi
