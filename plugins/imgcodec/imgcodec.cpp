//
// imgcodec.cpp — xi.imgcodec, the FIRST OFFICIAL LIB PLUGIN (docs/new_gen/14).
//
// A lib plugin has NO DATA PLANE: it never emits and nothing routes to it. On
// create it registers capabilities with the host (get_interface
// "xi.cap.provider"@1) and consumers call them by NAME through the host
// forwarding funnel (get_interface "xi.cap"@1) — never through this DLL's
// vtable.
//
// Capabilities (name-only registry; versioning rides IN the request pack):
//
//   "xi.jpeg.encode"  — Pack in:  image "image" (1/3/4-channel 8-bit),
//                                 optional i64 "quality" (1..100, default from
//                                 config), optional i64 "$v" (supported: 1),
//                                 or bool "$probe": true (answers $versions).
//                       Pack out: bin "jpeg" (the encoded bytes), i64
//                                 "cache_hit" (1 = served from the dedup memo
//                                 cache), i64 "encodes" (lifetime encode
//                                 count — the dedup proof counter), i64 "hits".
//                       DEDUP: keyed by the image's CONTENT identity (FNV-1a
//                       over pixels + dims, the same identity the host's
//                       xi.preview cache uses — sealed-pack pool handles are
//                       not exposed through xi_pack_image, so content hash is
//                       the stable identity available pre-v12) + quality. Two
//                       consumers asking for the same sealed image encode ONCE
//                       and receive byte-identical JPEG.
//
//   "xi.image.decode" — Pack in:  bin "data" (PNG/JPEG/BMP/TGA/GIF/PSD/HDR/PIC
//                                 — stb_image, mirroring host_api->
//                                 read_image_file's format set: this
//                                 capability is that field's designated
//                                 eviction target at v12; the host field
//                                 stays untouched until then), optional "$v" /
//                                 "$probe" as above.
//                       Pack out: image "image" + i64 w/h/c.
//
// Contract failures are NORMAL sealed packs carrying the $fault entries
// (pack_contract convention) — the funnel's rc stays 0; consumers route the
// $fault. Hard internal failures return XI_PACK_NULL.
//
// ENCODER CHOICE (pilot): stb_image_write, vendored in backend/vendor and
// compiled into this DLL via backend/src/stb_impl.cpp (the record_save
// pattern). Deterministic bytes, zero external deps. libjpeg-turbo is NOT
// actually deployed beside the backend today (XINSP2_HAS_TURBOJPEG is an
// opt-in cmake switch expecting an external install), so the doc-14 "turbo is
// already there" assumption does not hold in-tree; swapping this plugin's
// encoder to turbojpeg later is invisible to every consumer — that is the
// point of the capability boundary.
//
// THREAD SAFETY: handlers arrive concurrently from multiple dispatch threads
// (the funnel does NOT serialize — the provider contract). The memo cache is
// mutex-guarded; counters are atomics; config is read through the same mutex.
//
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// stb prototypes (implementation compiled in via backend/src/stb_impl.cpp).
extern "C" int stbi_write_jpg_to_func(
    void (*func)(void* context, void* data, int size), void* context,
    int x, int y, int comp, const void* data, int quality);
extern "C" unsigned char* stbi_load_from_memory(
    const unsigned char* buffer, int len, int* x, int* y,
    int* channels_in_file, int desired_channels);
extern "C" void stbi_image_free(void* retval_from_stbi_load);

namespace {
constexpr const char* kVersions = "1";   // supported "$v" range, both capabilities
}

class ImgCodec : public xi::Plugin {
public:
    ImgCodec(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        pk_ = pack_iface();   // xi.pack@1 (cached by the base)
        if (host && host->get_interface) {
            provider_ = static_cast<const xi_cap_provider_v1*>(
                host->get_interface("xi.cap.provider", 1));
        }
        if (!pk_ || !provider_) {
            // Inert on a host without the planes (e.g. certify probes an older
            // table): loadable, but it provides nothing and says so.
            status("imgcodec: host lacks xi.pack@1/xi.cap.provider@1 — no capabilities registered");
            return;
        }
        int32_t r1 = provider_->register_capability("xi.jpeg.encode",  &h_encode, this);
        int32_t r2 = provider_->register_capability("xi.image.decode", &h_decode, this);
        registered_ = (r1 == XI_CAP_REG_OK && r2 == XI_CAP_REG_OK);
        if (registered_) {
            status("imgcodec: providing xi.jpeg.encode, xi.image.decode");
        } else {
            char m[128];
            std::snprintf(m, sizeof(m),
                          "imgcodec: registration failed (encode=%d decode=%d)",
                          r1, r2);
            status(m);
        }
    }

    // Well-behaved lib: unregister on destroy. (The host's adapter-dtor owner
    // sweep backstops a plugin that forgets — proven by cap_plane_test.)
    ~ImgCodec() override {
        if (provider_ && registered_) {
            provider_->unregister_capability("xi.jpeg.encode", this);
            provider_->unregister_capability("xi.image.decode", this);
        }
    }

    // -- control surface ------------------------------------------------------
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string command = p["command"].as_string();
        if (command == "stats") {
            // The dedup proof counter: tests/QA read encodes vs hits here.
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "{\"encodes\":%lld,\"hits\":%lld,\"decodes\":%lld,"
                "\"cache_entries\":%zu,\"registered\":%s}",
                (long long)encodes_.load(), (long long)hits_.load(),
                (long long)decodes_.load(), cache_size_(),
                registered_ ? "true" : "false");
            return buf;
        }
        if (command == "clear_cache") {
            std::lock_guard<std::mutex> lk(mu_);
            cache_.clear();
            order_.clear();
            return "{\"ok\":true}";
        }
        return exchange_unknown_command(command);
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
                      "{\"quality\":%d,\"cache_max\":%d}", quality_, cache_max_);
        return buf;
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        quality_   = clamp_q_(p["quality"].as_int(quality_));
        cache_max_ = p["cache_max"].as_int(cache_max_);
        if (cache_max_ < 1) cache_max_ = 1;
        return true;
    }

private:
    // -- $v / $probe convention -----------------------------------------------
    // Returns a sealed reply for the transport-level conventions ($probe / bad
    // $v), or XI_PACK_NULL when the request should proceed as version `v`.
    xi_pack_handle version_gate_(xi_pack_handle in) const {
        int32_t probe = 0;
        if (pk_->get_bool && pk_->get_bool(in, "$probe", &probe) && probe) {
            xi_pack_builder b = pk_->builder_new();
            pk_->builder_add_str(b, "$versions", kVersions,
                                 (int32_t)std::strlen(kVersions));
            return pk_->builder_seal(b);
        }
        int64_t v = 1;                       // absent $v = the documented default
        pk_->get_i64(in, "$v", &v);
        if (v != 1) {
            xi_pack_builder b = pk_->builder_new();
            pk_->builder_add_str(b, "$fault", "unsupported_version", 19);
            pk_->builder_add_str(b, "$fault_key", "$v", 2);
            pk_->builder_add_str(b, "$versions", kVersions,
                                 (int32_t)std::strlen(kVersions));
            return pk_->builder_seal(b);
        }
        return XI_PACK_NULL;
    }

    xi_pack_handle fault_(const char* code, const char* key, const char* detail) const {
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_str(b, "$fault", code, (int32_t)std::strlen(code));
        pk_->builder_add_str(b, "$fault_key", key, (int32_t)std::strlen(key));
        pk_->builder_add_str(b, "$fault_detail", detail, (int32_t)std::strlen(detail));
        return pk_->builder_seal(b);
    }

    static int clamp_q_(int q) { return q < 1 ? 1 : (q > 100 ? 100 : q); }

    // -- xi.jpeg.encode ---------------------------------------------------------
    xi_pack_handle encode_(xi_pack_handle in) {
        if (xi_pack_handle early = version_gate_(in)) return early;

        xi_pack_image img{};
        if (!pk_->get_image(in, "image", &img) || !img.pixels)
            return fault_("missing_input", "image",
                          "xi.jpeg.encode: required image entry 'image' is missing");
        if (img.channels != 1 && img.channels != 3 && img.channels != 4)
            return fault_("wrong_type", "image",
                          "xi.jpeg.encode: 'image' must be 1/3/4-channel 8-bit");

        int q;
        {
            std::lock_guard<std::mutex> lk(mu_);
            q = quality_;
        }
        q = clamp_q_((int)pi64_(in, "quality", q));

        // Content identity + params — the memo key (see header note).
        const uint64_t key = content_key_(img, q);

        std::shared_ptr<const std::vector<uint8_t>> jpeg;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = cache_.find(key);
            if (it != cache_.end()) { jpeg = it->second; hit = true; }
        }
        if (hit) {
            hits_.fetch_add(1, std::memory_order_relaxed);
        } else {
            auto fresh = std::make_shared<std::vector<uint8_t>>();
            auto writer = [](void* ctx, void* data, int size) {
                auto* v = static_cast<std::vector<uint8_t>*>(ctx);
                auto* p = static_cast<uint8_t*>(data);
                v->insert(v->end(), p, p + size);
            };
            // xi_pack_image pixels are contiguous (w*h*c) — stb's layout.
            if (!stbi_write_jpg_to_func(writer, fresh.get(),
                                        img.width, img.height, img.channels,
                                        img.pixels, q) ||
                fresh->empty()) {
                return fault_("encode_failed", "image",
                              "xi.jpeg.encode: stb encoder failed");
            }
            encodes_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(mu_);
            // Two threads may have encoded the same key concurrently — first
            // one in wins; both produced identical bytes (deterministic
            // encoder), so either answer is correct.
            if (cache_.emplace(key, fresh).second) {
                order_.push_back(key);
                while ((int)order_.size() > cache_max_) {
                    cache_.erase(order_.front());
                    order_.pop_front();
                }
            }
            jpeg = fresh;
        }

        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_bin(b, "jpeg", jpeg->data(), (int32_t)jpeg->size());
        pk_->builder_add_i64(b, "cache_hit", hit ? 1 : 0);
        pk_->builder_add_i64(b, "encodes", (int64_t)encodes_.load(std::memory_order_relaxed));
        pk_->builder_add_i64(b, "hits", (int64_t)hits_.load(std::memory_order_relaxed));
        return pk_->builder_seal(b);
    }

    // -- xi.image.decode --------------------------------------------------------
    xi_pack_handle decode_(xi_pack_handle in) {
        if (xi_pack_handle early = version_gate_(in)) return early;

        const void* ptr = nullptr; int32_t len = 0;
        if (!pk_->get_bin(in, "data", &ptr, &len) || !ptr || len <= 0)
            return fault_("missing_input", "data",
                          "xi.image.decode: required bin entry 'data' is missing");

        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(
            static_cast<const unsigned char*>(ptr), len, &w, &h, &comp, 0);
        if (px && comp == 2) {
            // gray+alpha has no pack image tag — re-decode as RGB.
            stbi_image_free(px);
            px = stbi_load_from_memory(static_cast<const unsigned char*>(ptr),
                                       len, &w, &h, &comp, 3);
            comp = 3;
        }
        if (!px || w <= 0 || h <= 0 ||
            (comp != 1 && comp != 3 && comp != 4)) {
            if (px) stbi_image_free(px);
            return fault_("decode_failed", "data",
                          "xi.image.decode: unsupported or corrupt image bytes");
        }
        decodes_.fetch_add(1, std::memory_order_relaxed);

        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_image(b, "image", w, h, comp, px);
        pk_->builder_add_i64(b, "w", w);
        pk_->builder_add_i64(b, "h", h);
        pk_->builder_add_i64(b, "c", comp);
        stbi_image_free(px);
        return pk_->builder_seal(b);
    }

    // -- helpers ---------------------------------------------------------------
    int64_t pi64_(xi_pack_handle f, const char* key, int64_t d) const {
        int64_t v = d;
        pk_->get_i64(f, key, &v);
        return v;
    }
    static uint64_t content_key_(const xi_pack_image& img, int q) {
        const size_t n = (size_t)img.width * (size_t)img.height * (size_t)img.channels;
        uint64_t key = 1469598103934665603ull;              // FNV-1a offset basis
        const uint8_t* p = static_cast<const uint8_t*>(img.pixels);
        for (size_t i = 0; i < n; ++i) { key ^= p[i]; key *= 1099511628211ull; }
        key ^= ((uint64_t)img.width << 40) ^ ((uint64_t)img.height << 16)
             ^ (uint64_t)(img.channels * 1000 + q);
        return key;
    }
    size_t cache_size_() const {
        std::lock_guard<std::mutex> lk(mu_);
        return cache_.size();
    }

    // The registered pack-door-shaped handlers (funnel-invoked, concurrent).
    // In-plugin catch: the boundary is noexcept in practice (the XI_PLUGIN_IMPL
    // defense-in-depth); XI_PACK_NULL = hard failure sentinel.
    static xi_pack_handle h_encode(void* self, xi_pack_handle in) {
        try { return static_cast<ImgCodec*>(self)->encode_(in); }
        catch (...) { return XI_PACK_NULL; }
    }
    static xi_pack_handle h_decode(void* self, xi_pack_handle in) {
        try { return static_cast<ImgCodec*>(self)->decode_(in); }
        catch (...) { return XI_PACK_NULL; }
    }

    const xi_pack_v1*         pk_       = nullptr;
    const xi_cap_provider_v1* provider_ = nullptr;
    bool                      registered_ = false;

    mutable std::mutex mu_;   // cache_ / order_ / quality_ / cache_max_
    std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> cache_;
    std::deque<uint64_t> order_;      // FIFO rotation (the host xi.preview pattern)
    int quality_   = 85;
    int cache_max_ = 32;

    std::atomic<int64_t> encodes_{0};   // misses (real encode work) — dedup proof
    std::atomic<int64_t> hits_{0};
    std::atomic<int64_t> decodes_{0};
};

XI_PLUGIN_IMPL(ImgCodec)
