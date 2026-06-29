// preview_sink.cpp — multi-group "view a script's output via a plugin", post-VAR.
//
// VAR + the core vars/preview wire were removed (branch refactor/remove-var-core).
// A script surfaces what it wants to view by pushing a Record into this sink,
// tagged with a preview-group id (pg_id) — per stage, per thread, per camera, ...
//
//   #include "preview_api.hpp"          // ships with this plugin
//   xi::preview::Sink pv;
//   xi::Record r; pvar(r, "score", s); pvar(r, "edges", im);  // value + image, in order
//   pv.process("bright", r);
//
// Each pg_id keeps its OWN latest record + images. A UI tabs between groups and
// LAZILY fetches image pixels only when a preview is expanded:
//   list_groups               -> { groups: { pg: {seen, image_count}, ... } }  (tabs)
//   get {pg}                  -> { data($layout+values), image_count }         (collapsed: NO pixels)
//   get_image {pg, key}       -> { found, w, h, channels, b64 }                (expand: fetch pixels)
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <map>
#include <mutex>
#include <string>

namespace {
// Minimal base64 (raw image bytes -> ascii) so get_image can ride the text
// exchange channel. A production preview plugin would JPEG-encode + use a binary
// push channel; raw+base64 keeps this demo dependency-free.
std::string b64(const uint8_t* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = p[i] << 16;
        if (i + 1 < n) b |= p[i + 1] << 8;
        if (i + 2 < n) b |= p[i + 2];
        o.push_back(T[(b >> 18) & 63]);
        o.push_back(T[(b >> 12) & 63]);
        o.push_back(i + 1 < n ? T[(b >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? T[b & 63] : '=');
    }
    return o;
}
}  // namespace

class PreviewSink : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    static constexpr const char* kPgKey = "$pg";

    xi::Record process(const xi::Record& in) override {
        const std::string data = in.data_json();
        std::string pg = xi::Json::parse(data)[kPgKey].as_string("default");

        std::lock_guard<std::mutex> lk(mu_);
        Group& g = groups_[pg];
        g.data = data;
        g.images.clear();
        // Input images are valid only for THIS call — OWN a deep copy so the UI
        // can fetch them later.
        for (auto& [k, img] : in.images())
            if (!img.empty())
                g.images[k] = xi::Image(img.width, img.height, img.channels, img.data());
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
                    .set("seen", (int64_t)g.seen)
                    .set("image_count", (int)g.images.size()));
            return xi::Json::object().set("count", (int)groups_.size())
                                     .set("groups", groups).dump();
        }
        if (c == "get" || c == "get_latest") {
            // Collapsed view: layout + values + image_count, but NO pixels.
            const std::string pg = p["pg"].as_string("default");
            auto it = groups_.find(pg);
            if (it == groups_.end())
                return xi::Json::object().set("found", false).set("pg", pg).dump();
            return xi::Json::object()
                .set("found", true).set("pg", pg)
                .set("seen", (int64_t)it->second.seen)
                .set("data", it->second.data.empty() ? std::string("{}") : it->second.data)
                .set("image_count", (int)it->second.images.size()).dump();
        }
        if (c == "get_image") {
            // Lazy fetch: only sent when the UI expands a preview.
            const std::string pg  = p["pg"].as_string("default");
            const std::string key = p["key"].as_string();
            auto it = groups_.find(pg);
            if (it != groups_.end()) {
                auto im = it->second.images.find(key);
                if (im != it->second.images.end()) {
                    const xi::Image& g = im->second;
                    return xi::Json::object()
                        .set("found", true).set("pg", pg).set("key", key)
                        .set("w", g.width).set("h", g.height).set("channels", g.channels)
                        .set("b64", b64(g.data(), g.size())).dump();
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
        return xi::Json::object().set("count", (int)groups_.size())
                                 .set("groups", names).dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    struct Group {
        std::string                       data = "{}";
        long long                         seen = 0;
        std::map<std::string, xi::Image>  images;   // owned deep copies, fetched lazily
    };
    mutable std::mutex            mu_;
    std::map<std::string, Group>  groups_;
};

XI_PLUGIN_IMPL(PreviewSink)
