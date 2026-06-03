//
// cached_image_source.cpp — a webui replay cache.
//
// Composed in a script as `cache.process(img)` (the `input = cache(input)`
// pattern): every image fed through it is stored in a ring of N, each with a
// cache-owned slot id. Its instance UI shows the ring as a clickable thumbnail
// grid; clicking one REPLAYS it — emit_trigger(name, XI_TRIGGER_NULL, [cached]) →
// a fresh-id pass runs on that cached frame. This is the "review history / re-run
// a past input" half of the HDevelop-style loop (local_image_source is the
// pick-a-file half).
//
// Design (see project_cache_replay_model): the cache mints its OWN ids for what
// it stores (the slot counter), and replay dispatches with a FRESH trigger id
// (XI_TRIGGER_NULL → bus auto-mints). The original capture's id, if any, is not
// reused — every emitter owns its dispatch id.
//
// process(input):  caches input image "frame" (or the first image), returns it.
// exchange():
//   "get_status"        -> { capacity, count, frames:[{slot,w,h,thumb}], ... }
//   "set_capacity"{value}
//   "clear"
//   "replay" {index}    -> emit the cached frame at grid position `index`
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_image.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {
std::string base64(const std::vector<uint8_t>& d) {
    static const char* a =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        unsigned n = d[i] << 16;
        if (i + 1 < d.size()) n |= d[i + 1] << 8;
        if (i + 2 < d.size()) n |= d[i + 2];
        o.push_back(a[(n >> 18) & 63]);
        o.push_back(a[(n >> 12) & 63]);
        o.push_back(i + 1 < d.size() ? a[(n >> 6) & 63] : '=');
        o.push_back(i + 2 < d.size() ? a[n & 63] : '=');
    }
    return o;
}
} // namespace

class CachedImageSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Cache whatever image is fed through us, then pass it on unchanged.
    xi::Record process(const xi::Record& input) override {
        xi::Image img = input.has_image("frame") ? input.get_image("frame")
                                                 : first_image_(input);
        if (!img.empty()) store_(img);
        return xi::Record().image("frame", img).set("cached_count", (int)ring_.size());
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_capacity") {
            capacity_ = std::max(1, std::min(64, p["value"].as_int(capacity_)));
            while ((int)ring_.size() > capacity_) ring_.pop_front();
        } else if (command == "clear") {
            ring_.clear();
        } else if (command == "replay") {
            replay_(p["index"].as_int(-1));
        }
        return status_();
    }

    std::string get_def() const override { return const_cast<CachedImageSource*>(this)->status_(); }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        capacity_ = std::max(1, std::min(64, p["capacity"].as_int(capacity_)));
        return true;
    }

private:
    struct Slot { int id; xi::Image img; };   // img OWNS its bytes (copied on store)
    std::deque<Slot> ring_;
    int capacity_   = 12;
    int next_id_    = 1;     // cache-owned slot id (NOT a trigger id)
    int replayed_   = 0;

    xi::Image first_image_(const xi::Record& r) {
        for (auto& [k, im] : r.images()) if (!im.empty()) return im;
        return {};
    }

    void store_(const xi::Image& src) {
        // Copy into an owned heap image so it survives past this pass (the input
        // is a view into pool memory released when the trigger ends).
        xi::Image owned(src.width, src.height, src.channels, src.data());
        ring_.push_back({ next_id_++, std::move(owned) });
        while ((int)ring_.size() > capacity_) ring_.pop_front();
    }

    void replay_(int index) {
        if (index < 0 || index >= (int)ring_.size() || !host()) return;
        const xi::Image& img = ring_[(size_t)index].img;
        if (img.empty()) return;
        xi_image_handle h = host()->image_create(img.width, img.height, img.channels);
        if (!h) return;
        std::memcpy(host()->image_data(h), img.data(), img.size());
        xi_record_image rec{ "frame", h };
        host()->emit_trigger(name().c_str(), XI_TRIGGER_NULL, /*ts=*/0, &rec, 1);
        host()->image_release(h);
        ++replayed_;
    }

    std::string status_() {
        auto frames = xi::Json::array();
        for (auto& s : ring_) {
            auto e = xi::Json::object();
            e.set("slot", s.id).set("w", s.img.width).set("h", s.img.height)
             .set("thumb", thumb_(s.img));
            frames.push(e);
        }
        return xi::Json::object()
            .set("capacity", capacity_)
            .set("count", (int)ring_.size())
            .set("replayed", replayed_)
            .set("frames", frames)
            .dump();
    }

    std::string thumb_(const xi::Image& img) {
        if (img.empty()) return "";
        const int TW = 140;
        cv::Mat src = img.as_cv_mat(), small;
        if (img.width > TW) {
            int th = std::max(1, img.height * TW / img.width);
            cv::resize(src, small, cv::Size(TW, th), 0, 0, cv::INTER_AREA);
        } else src.copyTo(small);
        cv::Mat bgr;
        if (small.channels() == 3) cv::cvtColor(small, bgr, cv::COLOR_RGB2BGR);
        else                       bgr = small;
        std::vector<uint8_t> jpeg;
        std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 60 };
        if (cv::imencode(".jpg", bgr, jpeg, params))
            return "data:image/jpeg;base64," + base64(jpeg);
        return "";
    }
};

XI_PLUGIN_IMPL(CachedImageSource)
