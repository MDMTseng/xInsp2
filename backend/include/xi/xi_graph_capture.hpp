#pragma once
//
// xi_graph_capture.hpp — opt-in pipeline dataflow-graph capture (stage 2).
//
// OFF by default → zero hot-path cost. When enabled (cmd:graph_capture), each
// xi::use().process() call records one entry; snapshot() reconstructs dataflow
// EDGES by image-handle identity: if instance A's output image handle is later
// fed as instance B's input, that's an A→B edge. "Last producer wins" so a
// recycled pool slot attributes to its most recent writer.
//
// What this does NOT see: data the script pulls out of a pack into a plain
// C++ / cv::Mat var, computes on, and feeds back — once it leaves the pack its
// provenance is gone (taint-tracking through OpenCV is infeasible). So image
// edges are accurate; scalar/JSON flow through script math is not traced.
//
// Extracted from service_main.cpp: a self-contained opt-in diagnostic, not part
// of the dispatch core. The host keeps only the inline `enabled()` guard on the
// hot path (a relaxed atomic load — identical cost to before) and formats the
// snapshot to wire JSON; capture + the edge-reconstruction algorithm live here.
//
// TODO(linux): plain STL only — already portable.
//
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xi {

class GraphCapture {
public:
    static GraphCapture& instance() {
        static GraphCapture g;
        return g;
    }

    // Hot-path guard: a relaxed atomic load. Must stay cheap — callers check
    // this inline before touching anything else so the disabled path is free.
    bool enabled() const { return on_.load(std::memory_order_relaxed); }

    // Toggle capture. Enabling clears any prior recording.
    void set(bool on) {
        { std::lock_guard<std::mutex> lk(mu_); calls_.clear(); }
        on_.store(on, std::memory_order_relaxed);
    }

    // Record one dispatch call's input/output image handles + keys by plain
    // identity (no data-plane struct dependency): the caller passes the instance
    // name and the in/out image handles with their carried keys. Called only when
    // enabled(); handles are still valid at the call site. A null key array (or a
    // null slot in it) is treated as an empty key.
    void record(const char* name,
                const uint64_t* in_handles, const char* const* in_keys, int in_n,
                const uint64_t* out_handles, const char* const* out_keys, int out_n) {
        Call call;
        call.instance = name ? name : "";
        for (int i = 0; i < in_n; ++i) {
            call.in_handles.push_back(in_handles[i]);
            const char* k = (in_keys && in_keys[i]) ? in_keys[i] : "";
            call.in_keys.push_back(k);
        }
        for (int i = 0; i < out_n; ++i) {
            call.out_handles.push_back(out_handles[i]);
            const char* k = (out_keys && out_keys[i]) ? out_keys[i] : "";
            call.out_keys.push_back(k);
        }
        std::lock_guard<std::mutex> lk(mu_);
        calls_.push_back(std::move(call));
        // Bound the buffer so capture left on in continuous/production mode can't
        // grow without limit. The dataflow topology repeats every frame, so a ring
        // of recent calls still reconstructs the full graph; drop the oldest.
        while (calls_.size() > kMaxCalls) calls_.pop_front();
    }

    struct Edge {
        std::string              from;
        std::string              to;
        std::vector<std::string> keys;   // output keys carried along this edge
    };
    struct Snapshot {
        bool                     capturing = false;
        std::vector<std::string> ran;     // instances that actually ran (run order)
        std::vector<Edge>        edges;    // dataflow edges (deterministic order)
    };

    // Reconstruct dataflow edges from the recorded calls by image-handle
    // identity (A's output handle later used as B's input → A→B). "Last
    // producer wins". The host formats this to wire JSON.
    Snapshot snapshot() const {
        std::vector<Call> calls;
        { std::lock_guard<std::mutex> lk(mu_); calls.assign(calls_.begin(), calls_.end()); }

        std::unordered_map<uint64_t, std::pair<std::string, std::string>> producer;
        std::map<std::pair<std::string, std::string>, std::unordered_set<std::string>> edge_keys;
        Snapshot snap;
        snap.capturing = enabled();
        std::unordered_set<std::string> ran_seen;
        for (auto& c : calls) {
            if (ran_seen.insert(c.instance).second) snap.ran.push_back(c.instance);
            for (size_t i = 0; i < c.in_handles.size(); ++i) {
                auto it = producer.find(c.in_handles[i]);
                if (it != producer.end() && it->second.first != c.instance)
                    edge_keys[{ it->second.first, c.instance }].insert(it->second.second);
            }
            for (size_t i = 0; i < c.out_handles.size(); ++i)
                producer[c.out_handles[i]] = { c.instance, c.out_keys[i] };
        }
        for (auto& [ft, keys] : edge_keys) {
            Edge e;
            e.from = ft.first;
            e.to   = ft.second;
            e.keys.assign(keys.begin(), keys.end());
            snap.edges.push_back(std::move(e));
        }
        return snap;
    }

private:
    struct Call {
        std::string              instance;
        std::vector<uint64_t>    in_handles, out_handles;
        std::vector<std::string> in_keys,    out_keys;
    };
    // Plenty to reconstruct any real pipeline's topology (which repeats per
    // frame) while bounding memory if capture is accidentally left enabled.
    static constexpr size_t kMaxCalls = 8192;
    std::atomic<bool>  on_{false};
    mutable std::mutex mu_;
    std::deque<Call>   calls_;   // guarded by mu_ (ring: oldest dropped past kMaxCalls)
};

} // namespace xi
