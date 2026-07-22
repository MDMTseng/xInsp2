#pragma once
//
// live_view.hpp — the NATIVE half of the `live-view` pluginlet (doc 37).
//
// xi::pluginlet::LiveView: a plugin's self-owned live-preview egress, built on
// the xi::Derived gate/dedup cell (xi_reactive.hpp). PARASITIC (doc 37): it holds
// no owner identity and registers no capability — its allocations bill the host
// plugin, its faults are the host's, its lifecycle is the host's. It only ever
// reaches the host through the already-published pack/cap planes.
//
// Author surface, in the host plugin:
//
//     #include <live-view/live_view.hpp>
//     xi::pluginlet::LiveView view_{host, "ui/" + name};   // in the ctor
//     ...
//     view_.publish(painted_image);                        // per processed frame
//
// It owns the whole fiddly protocol its UI half (live_view.ui.ts) speaks — the
// author never touches a channel string, a subscription probe, or a viewport
// message:
//
//     demand   = probe xi.ui.sink for {subscribed, viewport}   (expose relays the
//                browser's latest viewport into the probe reply; absent => full)
//     project  = crop -> downsample -> encode via xi.jpeg.encode (imgcodec)
//     sink     = push the encoded frame to xi.ui.sink (expose transport)
//
// The viewport comes back from the UI widget over the SAME channel and rides the
// probe reply, so Demand::window is REAL here (not the aspirational passenger it
// is in the bare core). Demand::window folds into the cell's dedup key, so a
// pan/zoom re-projects the same frame to the new crop. When expose does not relay
// a viewport, window==0 and this degrades cleanly to subscription-only gating —
// exactly the bare-core behaviour.
//
// This OVERLAPS ui_egress on purpose: it is the pluginlet demonstration of the
// policy ui_egress hard-codes as shared middleware, for a plugin that wants the
// policy (per-channel quality, a crop, a downsample) in its OWN hands.
//
#include <xi/xi_reactive.hpp>
#include <xi/xi_abi.h>
#include <xi/xi_image.hpp>
#include <xi/xi_jpeg_cap.hpp>       // xi::encode_via_capability
#include <xi/xi_pack_contract.hpp>  // xi::pack_contract::kChannel

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace xi::pluginlet {

// The keys the viewport round-trip uses — ONE definition, mirrored in
// toolbox/pluginlets/live-view/contract.ts. The UI widget sends upstream
//   {type:"viewport", channel, x, y, w, h}
// and expose returns the latest as "x,y,w,h" (i32 CSV) under kViewport in the
// xi.ui.sink probe reply.
namespace live_view_keys {
    inline constexpr const char* kSubscribed = "subscribed";
    inline constexpr const char* kViewport   = "viewport";   // "x,y,w,h" (i32 CSV)
}

struct Viewport {
    int x = 0, y = 0, w = 0, h = 0;
    bool valid() const { return w > 0 && h > 0; }
    uint64_t key() const {
        return valid() ? (((uint64_t)(uint16_t)x << 48) ^ ((uint64_t)(uint16_t)y << 32) ^
                          ((uint64_t)(uint16_t)w << 16) ^ (uint64_t)(uint16_t)h)
                       : 0;
    }
};

class LiveView {
public:
    using Frame = std::vector<uint8_t>;

    // max_edge caps the LONGER encoded edge (downsample target) so a 20 MP source
    // never ships full-res to a thumbnail viewport; 0 disables downsampling.
    LiveView(const xi_host_api* host, std::string channel, int quality = 80, int max_edge = 1024)
        : channel_(std::move(channel)), quality_(quality), max_edge_(max_edge),
          cell_(channel_,
                [this] { return demand_(); },
                [this](Frame& out) { return project_(out); },
                [this](const Frame& f) { sink_(f); }) {
        if (host && host->get_interface) {
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
            pk_  = static_cast<const xi_pack_v1*>(host->get_interface("xi.pack", 1));
        }
    }

    // Per processed frame. Retains the image, hashes its pixels as the dedup key,
    // and runs one tick. The gate (subscription) and viewport are pulled inside
    // demand_(); Demand::window (the viewport) folds into the cell's dedup key, so
    // publish() only hashes the PIXELS. Returns true iff a frame was served
    // (freshly encoded OR handed the last encode).
    bool publish(const Image& img) {
        if (!cap_ || !pk_ || img.empty() || !img.data()) return false;
        pending_ = img;   // a view is cheap; a plugin whose pixels don't outlive
                          // the call should hand an OWNING image (retain the pack,
                          // not a dangling handle) — doc 37's caveat.
        const uint64_t h = fnv1a(img.data(), (size_t)img.width * img.height * img.channels);
        return cell_.refresh(h).served();
    }

    const Derived<Frame>::Stats& stats() const { return cell_.stats(); }
    const Viewport&              viewport() const { return vp_; }

private:
    // demand: expose answers {subscribed, viewport?} for this channel. Absent
    // plane / absent expose / unsubscribed => 0 viewers => gate shut, no encode.
    Demand demand_() {
        vp_ = Viewport{};
        if (!cap_ || !pk_ || !cap_->available || !cap_->available("xi.ui.sink"))
            return Demand{};
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_str(b, pack_contract::kChannel, channel_.c_str(), (int32_t)channel_.size());
        xi_pack_handle req = pk_->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        const int32_t rc = cap_->call("xi.ui.sink", req, &rsp);
        pk_->release(req);
        if (rc < 0 || rsp == XI_PACK_NULL) { if (rsp != XI_PACK_NULL) pk_->release(rsp); return Demand{}; }
        int64_t sub = 0;
        pk_->get_i64(rsp, live_view_keys::kSubscribed, &sub);
        const char* vp = nullptr; int32_t vlen = 0;
        if (pk_->get_str(rsp, live_view_keys::kViewport, &vp, &vlen) && vp && vlen > 0)
            vp_ = parse_viewport_(std::string(vp, (size_t)vlen));
        pk_->release(rsp);
        if (sub == 0) return Demand{};
        return Demand{ 1, vp_.key() };   // window folds into the cell's dedup key
    }

    // project: the expensive step, reached only past the gate + dedup. Crop to
    // the viewport (clamped), downsample to max_edge, encode via xi.jpeg.encode.
    bool project_(Frame& out) {
        const Image& src = pending_;
        Image roi = vp_.valid() ? crop_(src, vp_) : src;
        if (roi.empty()) return false;
        Image scaled = downsample_(roi, max_edge_);
        return encode_via_capability(scaled.empty() ? roi : scaled, quality_, out);
    }

    // sink: hand the encoded bytes to expose's byte-blind transport.
    void sink_(const Frame& f) {
        if (!cap_ || !pk_ || f.empty()) return;
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_str(b, pack_contract::kChannel, channel_.c_str(), (int32_t)channel_.size());
        pk_->builder_add_bin(b, "frame", f.data(), (int32_t)f.size());
        xi_pack_handle req = pk_->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        cap_->call("xi.ui.sink", req, &rsp);
        pk_->release(req);
        if (rsp != XI_PACK_NULL) pk_->release(rsp);
    }

    // "x,y,w,h" i32 CSV -> Viewport. Malformed => invalid (full frame).
    static Viewport parse_viewport_(const std::string& s) {
        Viewport v; int* f[4] = {&v.x, &v.y, &v.w, &v.h}; int i = 0;
        size_t p = 0;
        while (i < 4 && p <= s.size()) {
            size_t c = s.find(',', p);
            std::string tok = s.substr(p, c == std::string::npos ? std::string::npos : c - p);
            *f[i++] = std::atoi(tok.c_str());
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (i != 4) return Viewport{};
        return v;
    }

    // Copy the clamped ROI into a fresh PACKED image (xi::Image has no ROI stride,
    // so a crop is a compaction, not a view — cheap: the ROI is small by design).
    static Image crop_(const Image& src, Viewport v) {
        int x = v.x < 0 ? 0 : v.x, y = v.y < 0 ? 0 : v.y;
        if (x >= src.width || y >= src.height) return Image{};
        int w = v.w, h = v.h;
        if (x + w > src.width)  w = src.width  - x;
        if (y + h > src.height) h = src.height - y;
        if (w <= 0 || h <= 0) return Image{};
        const int c = src.channels;
        Image dst(w, h, c);                       // fresh owned packed buffer
        const uint8_t* sp = src.read();
        uint8_t* dp = dst.write();
        const size_t srow = (size_t)src.width * c, drow = (size_t)w * c;
        for (int r = 0; r < h; ++r)
            std::memcpy(dp + (size_t)r * drow, sp + (size_t)(y + r) * srow + (size_t)x * c, drow);
        return dst;
    }

    // Nearest-neighbour downscale so the LONGER edge <= max_edge. Returns empty
    // (caller uses the source) when no downscale is needed or max_edge==0.
    static Image downsample_(const Image& src, int max_edge) {
        if (max_edge <= 0) return Image{};
        const int longer = src.width > src.height ? src.width : src.height;
        if (longer <= max_edge) return Image{};
        const int dw = src.width  * max_edge / longer;
        const int dh = src.height * max_edge / longer;
        if (dw <= 0 || dh <= 0) return Image{};
        const int c = src.channels;
        Image dst(dw, dh, c);
        const uint8_t* sp = src.read();
        uint8_t* dp = dst.write();
        const size_t srow = (size_t)src.width * c;
        for (int y = 0; y < dh; ++y) {
            const int sy = y * src.height / dh;
            const uint8_t* srowp = sp + (size_t)sy * srow;
            uint8_t* drowp = dp + (size_t)y * dw * c;
            for (int x = 0; x < dw; ++x) {
                const int sx = x * src.width / dw;
                const uint8_t* s = srowp + (size_t)sx * c;
                uint8_t* d = drowp + (size_t)x * c;
                for (int k = 0; k < c; ++k) d[k] = s[k];
            }
        }
        return dst;
    }

    std::string       channel_;
    int               quality_;
    int               max_edge_;
    const xi_cap_v1*  cap_ = nullptr;
    const xi_pack_v1* pk_  = nullptr;
    Image             pending_;
    Viewport          vp_;
    Derived<Frame>    cell_;
};

} // namespace xi::pluginlet
