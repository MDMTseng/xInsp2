#pragma once
//
// xi_reactive.hpp — xi::Derived<Out>: a demand-gated, dedup-memoized derivation
// cell for SDK-side plugin code, plus UiView, one worked application of it (the
// live-preview egress a plugin owns for itself).
//
// WHERE THIS LIVES. Entirely ABOVE the frozen C ABI — an SDK header, zero core
// change, no new xi_host_api slot. A plugin instantiates a Derived cell in its
// own translation unit; the cell only ever reaches the host through the already-
// published pack/cap planes (get_interface). Nothing here is privileged.
//
// THE PATTERN it captures (the repeated shape behind ui_egress / mock_camera's
// preview push / any "expensive projection nobody may be watching"):
//
//     gate on demand  ->  skip if nobody's looking      (no viewer  = no work)
//     dedup on input  ->  skip if the input is unchanged (same frame = no work)
//     project         ->  the one expensive step (encode / crop / downsample)
//     sink            ->  hand the result downstream
//
// It is the vision-d.md law rendered as a cell: "own each truth once, project it
// everywhere, GATE the projection so it cannot skip." The gate is lazy-pull
// (Solid/MobX flavour, not React's eager push): refresh() ASKS demand() each
// tick and does nothing when the answer is zero — so an unsubscribed channel
// costs one cheap probe and not one JPEG encode. Dedup is the same content-hash
// memo ui_egress keeps (FNV-1a over the input), lifted out of egress so any
// plugin gets it for free.
//
// WHAT IT IS NOT (honest caveats carried from the design discussion):
//   * Not a signal graph. One cell, one output, explicit deps passed to
//     refresh(). No auto-tracked dependency web, no glitch/topological-sort
//     machinery — that complexity is the thing we are deliberately NOT buying.
//   * demand() is a plain function the caller supplies. UiView's demand is a
//     subscription probe; a viewport-scoped demand (viewers>0 AND a crop window)
//     is aspirational — the ABI carries no viewport channel today, so UiView
//     gates on subscription only and leaves Demand::window as the seam for it.
//   * The dedup key is the INPUT hash the caller computes. The cell does not
//     hash for you (it can't see your input's layout) — you pass fnv1a(pixels)
//     or whatever identity is cheaper than the projection you are gating.
//
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace xi {

// FNV-1a, 64-bit — the same content-identity primitive ui_egress uses for its
// encode memo. Cheap enough to run per frame, strong enough to gate an encode.
inline uint64_t fnv1a(const void* data, size_t len, uint64_t seed = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// How much downstream interest exists THIS tick. viewers<=0 shuts the gate.
// `window` is the viewport seam (0 = whole frame / unknown); no ABI channel
// feeds it yet, so today it only rides along for a future viewport-scoped cell.
struct Demand {
    int      viewers = 0;
    uint64_t window  = 0;   // aspirational: an ROI/zoom identity; 0 = full/none
};

// A single demand-gated, dedup-memoized derivation of one Out value.
//
//   demand()          -> Demand   : is anyone watching? (lazy pull, per tick)
//   project(Out&)     -> bool     : the expensive step; false => Failed
//   sink(const Out&)             : hand the fresh result downstream
//
// refresh(input_hash) runs one tick and reports which of the four things
// happened. The value is retained between ticks so Deduped can hand back the
// last projection without recomputing it.
template <class Out>
class Derived {
public:
    enum class Status { Suspended, Deduped, Derived, Failed };

    struct Result {
        Status     status = Status::Suspended;
        const Out* value  = nullptr;   // non-null only for Derived / Deduped
        bool changed() const { return status == Status::Derived; }
        bool served() const  { return status == Status::Derived || status == Status::Deduped; }
    };

    struct Stats {
        uint64_t ticks = 0, suspended = 0, deduped = 0, derived = 0, failed = 0;
    };

    using DemandFn  = std::function<Demand()>;
    using ProjectFn = std::function<bool(Out&)>;
    using SinkFn    = std::function<void(const Out&)>;

    Derived(std::string name, DemandFn demand, ProjectFn project, SinkFn sink = {})
        : name_(std::move(name)), demand_(std::move(demand)),
          project_(std::move(project)), sink_(std::move(sink)) {}

    // One tick. `input_hash` is the caller's cheap identity of the current input
    // (e.g. fnv1a over the source pixels) — the dedup key. Ordering is the whole
    // point: gate FIRST (so an unwatched channel never even reaches project),
    // dedup SECOND (so an unchanged input never re-projects), project LAST.
    Result refresh(uint64_t input_hash) {
        stats_.ticks++;

        const Demand d = demand_ ? demand_() : Demand{};
        if (d.viewers <= 0) { stats_.suspended++; return {Status::Suspended, nullptr}; }

        if (have_ && input_hash == last_hash_) {
            stats_.deduped++;
            return {Status::Deduped, &value_};
        }

        Out next{};
        if (!project_ || !project_(next)) { stats_.failed++; return {Status::Failed, nullptr}; }

        value_     = std::move(next);
        last_hash_ = input_hash;
        have_      = true;
        if (sink_) sink_(value_);
        stats_.derived++;
        return {Status::Derived, &value_};
    }

    // Drop the memo — the next refresh re-projects even on an unchanged input.
    // (Use when the projection PARAMETERS changed, e.g. a new viewport, so the
    // input hash is stale as an identity.)
    void invalidate() { have_ = false; last_hash_ = 0; }

    const Stats&       stats() const { return stats_; }
    const std::string& name()  const { return name_; }
    bool               primed() const { return have_; }

private:
    std::string name_;
    DemandFn    demand_;
    ProjectFn   project_;
    SinkFn      sink_;
    Out         value_{};
    uint64_t    last_hash_ = 0;
    bool        have_      = false;
    Stats       stats_;
};

} // namespace xi

// ---------------------------------------------------------------------------
// UiView — one worked application: a plugin's self-owned live-preview egress.
//
// It is exactly the Derived pattern wired to the real planes:
//     demand   = probe xi.ui.sink for `subscribed`   (expose's gate)
//     project  = encode the retained Image via xi.jpeg.encode (imgcodec)
//     sink     = hand the encoded frame to xi.ui.sink (expose transport)
// so a plugin that wants to own its preview policy writes:
//
//     UiView view(host, "ui/" + name);        // in the ctor
//     ...
//     view.publish(painted_image);            // per processed frame
//
// and gets the full "no viewer => no encode, same frame => no re-encode" gate
// for free, without going through the shared ui_egress timer. This OVERLAPS
// ui_egress on purpose — it is the SDK-level demonstration of the pattern
// ui_egress hard-codes, for plugins that want the policy in their own hands
// (a different quality per channel, a crop, a downsample) rather than the
// one-size middleware. It is deliberately behind XI_REACTIVE_UIVIEW so the
// pure Derived core above stays dependency-free for headless unit tests.
// ---------------------------------------------------------------------------
#ifdef XI_REACTIVE_UIVIEW
#include <vector>
#include <xi/xi_abi.h>
#include <xi/xi_image.hpp>
#include <xi/xi_jpeg_cap.hpp>     // xi::encode_via_capability
#include <xi/xi_pack_contract.hpp> // xi::pack_contract::kChannel

namespace xi {

class UiView {
public:
    using Frame = std::vector<uint8_t>;

    UiView(const xi_host_api* host, std::string channel, int quality = 80)
        : channel_(std::move(channel)), quality_(quality),
          cell_(channel_,
                [this] { return probe_demand_(); },
                [this](Frame& out) { return project_(out); },
                [this](const Frame& f) { sink_(f); }) {
        if (host && host->get_interface) {
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
            pk_  = static_cast<const xi_pack_v1*>(host->get_interface("xi.pack", 1));
        }
    }

    // Per processed frame. Retains the image for the (possibly deferred/deduped)
    // projection, hashes its pixels as the dedup key, and runs one tick. Returns
    // true iff a frame was served (freshly encoded OR handed the last encode).
    bool publish(const Image& img) {
        if (!cap_ || !pk_ || img.empty() || !img.data()) return false;
        pending_ = img;   // a view is cheap; a plugin that outlives the pixels
                          // should hand an owning Image (retain the pack, not a
                          // dangling handle) — the design's "retain pack not
                          // handle" caveat.
        const uint64_t h = fnv1a(img.data(), (size_t)img.width * img.height * img.channels);
        return cell_.refresh(h).served();
    }

    const Derived<Frame>::Stats& stats() const { return cell_.stats(); }

private:
    // demand: expose answers `subscribed` for this channel (0 when nobody's on
    // the socket). Absent plane / absent expose => 0 viewers => gate shut.
    Demand probe_demand_() {
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
        pk_->get_i64(rsp, "subscribed", &sub);
        pk_->release(rsp);
        return Demand{ sub != 0 ? 1 : 0, 0 };
    }

    // project: the expensive step, reached only past the gate + dedup. Encode
    // the retained image through xi.jpeg.encode (imgcodec / turbojpeg).
    bool project_(Frame& out) {
        return encode_via_capability(pending_, quality_, out);
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

    std::string       channel_;
    int               quality_;
    const xi_cap_v1*  cap_ = nullptr;
    const xi_pack_v1* pk_  = nullptr;
    Image             pending_;
    Derived<Frame>    cell_;
};

} // namespace xi
#endif // XI_REACTIVE_UIVIEW
