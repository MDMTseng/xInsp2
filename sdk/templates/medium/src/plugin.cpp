//
// {{NAME}} — "medium" template: image processor (Layer 1).
//
// The SAME xi::Plugin skeleton as the easy template, with two layers turned
// on: configurable params (get_def/set_def + an exchange RPC channel, all via
// xi::Json) and a live status() line. Its xi.pack@1 door reads an input image
// keyed "gray", thresholds it, and seals the results into the output pack:
//   image "binary"     — the thresholded result (uint8, 1 channel)
//   f64   "fg_pct"     — fraction of foreground pixels, 0.0 .. 1.0
//   i64   "threshold"  — the threshold value that was applied
//
// Demonstrates the blessed patterns: pool_image() for a zero-copy output +
// out.adopt_image() to hand it to the pack by refcount, the xi::as_cv_read /
// xi::as_cv_write bridges, out.fault() for the fail-loud missing-input path,
// xi::Json for parsing, and status() for the operator channel.
//
// xi_plugin_support.hpp is force-included (xi::Plugin, xi::PackIn/PackOut,
// xi::Image, pool_image, the cv bridges). xi::Json and the canonical
// xi::contract reason codes are the two extra headers a config plugin
// pulls in.
//

#include <xi/xi_json.hpp>
#include <xi/xi_mp.hpp>         // canonical msgpack Writer — the xi/image blob descriptor
#include <xi/xi_contract.hpp>   // canonical fail-loud reason codes for out.fault()

#include <atomic>
#include <string>

class {{CLASS}} : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // ---- Config: a single 'threshold' int, 0..255 -------------------------
    //
    // get_def() is what the UI renders + what project.json stores; set_def()
    // is its inverse. instance.json round-trips through this pair, so the keys
    // must match. Built and parsed with xi::Json — no hand-rolled string
    // scanning (the anti-pattern the SDK README warns against).
    //
    std::string get_def() const override {
        return xi::Json::object()
            .set("threshold", threshold_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        threshold_ = clamp_(p["threshold"].as_int(threshold_.load()));
        return true;
    }

    // ---- process: the xi.pack@1 door — image in → image out ----------------
    void process(xi::PackIn& in, xi::PackOut& out) override {
        // Fail loud: a missing/mis-typed required input is a NORMAL sealed
        // pack stamped with a $fault reason the caller routes to a verdict —
        // never a silent empty default. (in.image() returns nullopt when
        // "gray" is absent or not an image entry; no coercion.)
        auto src = in.image("gray");
        if (!src || !src->pixels) {
            out.fault(xi::contract::kMissingInput, "gray",
                      "{{NAME}}: required image 'gray' is missing");
            return;
        }

        // Read the INPUT through a non-owning view + as_cv_read (the pack's
        // pixels are a zero-copy pool span shared with other consumers — read
        // them, never mutate them). Collapse a colour input to gray first so the
        // threshold has a single channel to work on.
        xi::Image srcView = xi::Image::view(src->width, src->height, src->channels,
                                            static_cast<const uint8_t*>(src->pixels));
        cv::Mat srcMat = xi::as_cv_read(srcView);
        cv::Mat gray;
        if (srcMat.channels() == 1) gray = srcMat;
        else                        cv::cvtColor(srcMat, gray, cv::COLOR_BGR2GRAY);

        // Produce the output as a self-describing xi/image BLOB (spec 30): mint a
        // headed pool buffer, wrap its 64B-aligned payload as a cv::Mat, and let
        // cv::threshold write the binary image straight INTO it — then adopt the
        // buffer zero-copy (the pack addrefs; we drop our mint ref). No heap→pool
        // memcpy. (The frozen @1 out.adopt_image door adapter now copies, so an
        // in-tree producer mints the headed buffer directly.)
        const int w = src->width, h = src->height;
        xi::mp::Writer dw;                        // {"t":"xi/image","w","h","c","dt"}
        dw.map(5);
        dw.key("t");  dw.str("xi/image");
        dw.key("w");  dw.int_(w);
        dw.key("h");  dw.int_(h);
        dw.key("c");  dw.int_(1);
        dw.key("dt"); dw.str("u8");
        void* pp = nullptr;
        xi_image_handle bh = out.blob_mint(dw.bytes().data(), (int32_t)dw.bytes().size(),
                                           (int64_t)w * h, &pp);
        if (!bh || !pp) {
            out.fault("no_blob_plane", "binary", "{{NAME}}: host has no xi.pack@4 blob plane");
            return;
        }
        cv::Mat binMat(h, w, CV_8UC1, pp);        // aliases the blob payload
        cv::threshold(gray, binMat, (double)threshold_.load(), 255.0, cv::THRESH_BINARY);

        const double pixels = (double)w * h;
        const double fg_pct = pixels > 0 ? (double)cv::countNonZero(binMat) / pixels : 0.0;
        last_fg_pct_ = fg_pct;
        status("thr=" + std::to_string(threshold_.load()) +
               " fg=" + std::to_string(fg_pct));

        out.adopt_blob("binary", bh);
        host_->image_release(bh);                 // pack holds its own addref now
        out.f64("fg_pct",    fg_pct);
        out.i64("threshold", threshold_.load());
    }

    // ---- exchange: the UI / script RPC channel ----------------------------
    //
    // The UI panel posts { command: "set_threshold", value: N } (and a
    // "get_status" poll); the host wraps it into JSON and lands it here. We
    // parse with xi::Json, apply, and return the current status the UI renders.
    //
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_threshold")
            threshold_ = clamp_(p["value"].as_int(threshold_.load()));
        else if (command != "get_status" && !command.empty())
            return exchange_unknown_command(command);
        return xi::Json::object()
            .set("threshold",   threshold_.load())
            .set("last_fg_pct", last_fg_pct_.load())
            .dump();
    }

private:
    static int clamp_(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

    // process() runs on a dispatch worker while exchange()/set_def() run on the
    // host's control thread — atomics keep the shared config race-free without a
    // lock. (A plugin with richer coupled state guards it with a mutex instead;
    // see plugins/expose, or config_swap_probe for the frame-perfect swap.)
    std::atomic<int>    threshold_{128};
    std::atomic<double> last_fg_pct_{0.0};
};

XI_PLUGIN_IMPL({{CLASS}})
// Publish the xi.pack@1 pack-in/pack-out door — the host probes
// xi_plugin_get_interface("xi.pack", 1) to learn this plugin speaks packs.
// THE CUT (v12): this is the SOLE data-plane door, so a plugin that overrides
// process(PackIn&, PackOut&) MUST publish it. Always after XI_PLUGIN_IMPL.
XI_PLUGIN_PACK_DOOR({{CLASS}})
