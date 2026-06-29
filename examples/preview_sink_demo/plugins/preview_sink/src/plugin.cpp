// preview_sink.cpp — multi-group preview sink with LIVE binary image push (v8).
//
// A script surfaces output via xi::use("preview").process(Record{values + images},
// tagged with a preview-group id pg_id). This sink:
//   - pushes each image LIVE to the UI as a self-describing binary frame
//     (host_api->emit_binary, ABI v8) — JPEG, no base64, no poll;
//   - skips re-compressing an image it has already encoded (content-hash dedup —
//     the "same image, don't compress twice" rule the old core preview had);
//   - keeps the latest record per group for the collapsed tab view (get / list_groups),
//     and serves a still on demand via get_image (pull fallback).
//
// BINARY FRAME FORMAT (plugin -> WS; the UI mirrors this):
//   [0..3]  magic 'X','P','V','1'
//   [4..5]  width   u16 LE
//   [6..7]  height  u16 LE
//   [8]     channels u8
//   [9]     codec    u8   (1 = jpeg)
//   [10]    pg_len   u8
//   [11]    key_len  u8
//   [12..]  pg_id bytes, then key bytes, then the JPEG payload (rest of frame)
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <opencv2/imgcodecs.hpp>   // cv::imencode — opencv is on the plugin compile path

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

std::string b64(const uint8_t* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = p[i] << 16;
        if (i + 1 < n) b |= p[i + 1] << 8;
        if (i + 2 < n) b |= p[i + 2];
        o.push_back(T[(b >> 18) & 63]); o.push_back(T[(b >> 12) & 63]);
        o.push_back(i + 1 < n ? T[(b >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? T[b & 63] : '=');
    }
    return o;
}

void put_u16(std::vector<uint8_t>& f, uint16_t v) { f.push_back(v & 0xFF); f.push_back(v >> 8); }

std::vector<uint8_t> build_frame(const std::string& pg, const std::string& key,
                                 int w, int h, int c, const std::vector<uint8_t>& jpeg) {
    std::vector<uint8_t> f;
    f.reserve(12 + pg.size() + key.size() + jpeg.size());
    f.push_back('X'); f.push_back('P'); f.push_back('V'); f.push_back('1');
    put_u16(f, (uint16_t)w); put_u16(f, (uint16_t)h);
    f.push_back((uint8_t)c); f.push_back(1 /*jpeg*/);
    f.push_back((uint8_t)pg.size()); f.push_back((uint8_t)key.size());
    f.insert(f.end(), pg.begin(), pg.end());
    f.insert(f.end(), key.begin(), key.end());
    f.insert(f.end(), jpeg.begin(), jpeg.end());
    return f;
}

}  // namespace

class PreviewSink : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    static constexpr const char* kPgKey = "$pg";

    xi::Record process(const xi::Record& in) override {
        const std::string data = in.data_json();
        const std::string pg = xi::Json::parse(data)[kPgKey].as_string("default");

        std::lock_guard<std::mutex> lk(mu_);
        Group& g = groups_[pg];
        g.data = data;
        g.images.clear();
        for (auto& [key, img] : in.images()) {
            if (img.empty()) continue;
            // OWN a deep copy for the pull fallback (input handles are call-scoped).
            g.images[key] = xi::Image(img.width, img.height, img.channels, img.data());
            // Dedup + encode once, then push LIVE to the UI.
            const std::vector<uint8_t>& jpeg = encode_cached_(img);
            if (!jpeg.empty())
                emit_binary(build_frame(pg, key, img.width, img.height, img.channels, jpeg));
        }
        ++g.seen;
        return xi::Record().set("pg", pg).set("seen", (int64_t)g.seen)
                           .set("encodes", (int64_t)encodes_).set("dedup_hits", (int64_t)dedup_hits_);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string c = p["command"].as_string();
        std::lock_guard<std::mutex> lk(mu_);

        if (c == "list_groups") {
            auto groups = xi::Json::object();
            for (auto& [pg, g] : groups_)
                groups.set(pg.c_str(), xi::Json::object()
                    .set("seen", (int64_t)g.seen).set("image_count", (int)g.images.size()));
            return xi::Json::object().set("count", (int)groups_.size())
                .set("groups", groups)
                .set("encodes", (int64_t)encodes_).set("dedup_hits", (int64_t)dedup_hits_).dump();
        }
        if (c == "get" || c == "get_latest") {
            const std::string pg = p["pg"].as_string("default");
            auto it = groups_.find(pg);
            if (it == groups_.end())
                return xi::Json::object().set("found", false).set("pg", pg).dump();
            return xi::Json::object().set("found", true).set("pg", pg)
                .set("seen", (int64_t)it->second.seen)
                .set("data", it->second.data.empty() ? std::string("{}") : it->second.data)
                .set("image_count", (int)it->second.images.size()).dump();
        }
        if (c == "get_image") {  // pull fallback: a still as JPEG base64 (data URL)
            const std::string pg = p["pg"].as_string("default"), key = p["key"].as_string();
            auto it = groups_.find(pg);
            if (it != groups_.end()) {
                auto im = it->second.images.find(key);
                if (im != it->second.images.end()) {
                    const std::vector<uint8_t>& jpeg = encode_cached_(im->second);
                    return xi::Json::object().set("found", true).set("pg", pg).set("key", key)
                        .set("w", im->second.width).set("h", im->second.height)
                        .set("channels", im->second.channels)
                        .set("jpeg_b64", b64(jpeg.data(), jpeg.size())).dump();
                }
            }
            return xi::Json::object().set("found", false).set("pg", pg).set("key", key).dump();
        }
        if (c == "clear") groups_.clear();
        return xi::Json::object().set("count", (int)groups_.size()).dump();
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        auto names = xi::Json::array();
        for (auto& [pg, g] : groups_) { (void)g; names.push(pg); }
        return xi::Json::object().set("count", (int)groups_.size()).set("groups", names).dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    struct Group {
        std::string                       data = "{}";
        long long                         seen = 0;
        std::map<std::string, xi::Image>  images;   // deep copies for the pull fallback
    };

    // Encode an image to JPEG, reusing a cached result for an identical image
    // (content hash) so the same frame surfaced to many groups/keys compresses
    // ONCE. Bounded LRU-ish ring (evict oldest hash past kCacheCap). Caller holds mu_.
    const std::vector<uint8_t>& encode_cached_(const xi::Image& img) {
        const uint64_t h = fnv1a(img.data(), img.size());
        auto it = cache_.find(h);
        if (it != cache_.end()) { ++dedup_hits_; return it->second; }
        std::vector<uint8_t> jpeg;
        // as_cv_mat is a non-owning view over the image bytes; imencode reads only.
        cv::imencode(".jpg", img.as_cv_mat(), jpeg, { cv::IMWRITE_JPEG_QUALITY, 85 });
        ++encodes_;
        cache_order_.push_back(h);
        while (cache_order_.size() > kCacheCap) { cache_.erase(cache_order_.front()); cache_order_.pop_front(); }
        return cache_.emplace(h, std::move(jpeg)).first->second;
    }

    static constexpr size_t kCacheCap = 64;
    mutable std::mutex                          mu_;
    std::map<std::string, Group>                groups_;
    std::map<uint64_t, std::vector<uint8_t>>    cache_;        // content-hash -> jpeg
    std::deque<uint64_t>                        cache_order_;  // eviction order
    long long                                   encodes_ = 0, dedup_hits_ = 0;
};

XI_PLUGIN_IMPL(PreviewSink)
