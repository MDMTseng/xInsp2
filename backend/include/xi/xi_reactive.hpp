#pragma once
//
// xi_reactive.hpp — xi::Derived<Out>: a demand-gated, dedup-memoized derivation
// cell for SDK-side plugin code. The reusable CORE of the pluginlet model (doc
// 37). Its first worked application, the `live-view` pluginlet's native half
// (xi::pluginlet::LiveView), lives in pluginlets/live-view/live_view.hpp — this
// header stays dependency-free (functional/string only) so it unit-tests
// headless and any plugin can #include it without pulling the pack/cap planes.
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
    //
    // Demand::window folds INTO the dedup key: a projection scoped to a viewport
    // must re-run when the viewport changes even if the input pixels are
    // identical (pan/zoom the same frame → a different crop). window==0 (no/full
    // viewport) leaves the key = input_hash, so a subscription-only cell is
    // unaffected. This is what makes Demand::window meaningful at the core rather
    // than a passenger the application has to hash in by hand.
    Result refresh(uint64_t input_hash) {
        stats_.ticks++;

        const Demand d = demand_ ? demand_() : Demand{};
        if (d.viewers <= 0) { stats_.suspended++; return {Status::Suspended, nullptr}; }

        const uint64_t key = input_hash ^ (d.window * 0x9E3779B97F4A7C15ull);
        if (have_ && key == last_hash_) {
            stats_.deduped++;
            return {Status::Deduped, &value_};
        }

        Out next{};
        if (!project_ || !project_(next)) { stats_.failed++; return {Status::Failed, nullptr}; }

        value_     = std::move(next);
        last_hash_ = key;
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
