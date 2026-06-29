// preview_sink.cpp — multi-group preview sink with LIVE binary image push.
//
// A script surfaces output via xi::use("preview").process(Record{values + images},
// tagged with a preview-group id pg_id). This sink:
//   - JPEG-encodes each image through the HOST cache (host->compress_image, ABI
//     v9) — so the same frame compressed across plugins is encoded ONCE globally,
//     and this plugin needs no opencv/turbojpeg of its own;
//   - PUSHES each as a self-describing binary frame (host->emit_binary, ABI v8);
//   - keeps the latest record per group for the collapsed tab view + a pull still.
//
// BINARY FRAME FORMAT (plugin -> WS; the UI mirrors this):
//   [0..3] 'XPV1' | [4..5] u16 w | [6..7] u16 h | [8] u8 ch | [9] u8 codec(1=jpeg)
//   | [10] u8 pg_len | [11] u8 key_len | pg bytes | key bytes | jpeg payload
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

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
            g.images[key] = xi::Image(img.width, img.height, img.channels, img.data());  // own for pull
            std::vector<uint8_t> jpeg = compress_(img);   // host cache: encoded once globally
            if (!jpeg.empty())
                emit_binary(build_frame(pg, key, img.width, img.height, img.channels, jpeg));
        }
        ++g.seen;
        return xi::Record().set("pg", pg).set("seen", (int64_t)g.seen);
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
            return xi::Json::object().set("count", (int)groups_.size()).set("groups", groups).dump();
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
                    std::vector<uint8_t> jpeg = compress_(im->second);
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
        std::map<std::string, xi::Image>  images;
    };

    // JPEG-encode via the host cache (host->compress_image): identical images are
    // encoded once globally, and we don't link a codec ourselves.
    std::vector<uint8_t> compress_(const xi::Image& img) const {
        const xi_host_api* h = host();
        if (!h || !h->compress_image || img.empty()) return {};
        std::vector<uint8_t> jpeg(64 * 1024);
        int n = h->compress_image(img.data(), img.width, img.height, img.channels, 85,
                                  jpeg.data(), (int)jpeg.size());
        if (n < 0) {  // buffer too small — resize to needed and retry
            jpeg.resize((size_t)(-n));
            n = h->compress_image(img.data(), img.width, img.height, img.channels, 85,
                                  jpeg.data(), (int)jpeg.size());
        }
        if (n <= 0) return {};
        jpeg.resize((size_t)n);
        return jpeg;
    }

    mutable std::mutex            mu_;
    std::map<std::string, Group>  groups_;
};

XI_PLUGIN_IMPL(PreviewSink)
