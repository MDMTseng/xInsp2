// buffer_replay.cpp — replay as plugin composition.
//
// The host used to own record/replay; in the ABI-v6 dispatch model that's a
// plugin's job. buffer_replay is the reference for it: process(in) captures the
// incoming record into a bounded ring; an exchange command re-emits a buffered
// record via xi::emit_record so the script re-runs on it. That is the
// HDevelop-style hot-param loop — buffer a frame, tune a Param, "replay_last" to
// re-inspect the SAME frame with the new value, no camera re-grab.
//
//   in the inspect script:
//     auto t = xi::current_trigger();
//     xi::use("buffer").process(xi::Record().image("img", t.image("src")));  // capture
//     ... inspect with a tunable Param ...
//   then, from the UI / a test (cmd as an OBJECT, not a string):
//     exchange_instance("buffer", {"command":"replay_last"})  // re-inspect last frame
//
// replay_last / replay_all / replay emit back-to-back (as fast as dispatch will
// take them). replay_timed re-emits paced by the ORIGINAL inter-capture gaps
// (optionally time-scaled) on a background thread, reproducing the arrival
// cadence — the "timed" replay mode. It does NOT reproduce byte-identical
// ordering (that is on-disk deterministic replay, a separate feature); it
// reproduces load/timing shape.

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class BufferReplay : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    ~BufferReplay() override { stop_replay_(); }

    // Pass-through + capture. Buffering keeps frames ACROSS calls, but an input
    // image is only valid for THIS call (the host owns its handle for the
    // duration of process). So we OWN the pixels: deep-copy each image into a
    // heap-backed Image. The metadata doc rides the Record copy. We also stamp
    // a monotonic capture time so replay_timed can reproduce the cadence.
    xi::Record process(const xi::Record& in) override {
        xi::Record owned = in;
        for (auto& [k, img] : in.images())
            if (!img.empty())
                owned.image(k, xi::Image(img.width, img.height, img.channels, img.data()));

        std::lock_guard<std::mutex> lk(mu_);
        ring_.push_back(Entry{std::move(owned), mono_us_()});
        while ((int)ring_.size() > capacity_) ring_.pop_front();
        return xi::Record().set("buffered", (int64_t)ring_.size());
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto c = p["command"].as_string();

        if (c == "replay_timed") {
            // Paced replay of the buffered frames on a background thread, so the
            // exchange returns immediately. `speed` scales the recorded gaps
            // (>1 faster, default 1.0). `n` optionally limits to the last n.
            double speed = p["speed"].as_double(1.0); if (speed <= 0.0) speed = 1.0;
            int    k     = p["n"].as_int(-1);
            std::vector<Entry> snap;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const int n = (int)ring_.size();
                int start = (k > 0 && k < n) ? n - k : 0;
                for (int i = start; i < n; ++i) snap.push_back(ring_[(size_t)i]);
            }
            start_timed_(std::move(snap), speed);
            return get_def();
        }

        // Snapshot the records to replay UNDER the lock, then emit OUTSIDE it
        // (emit_record dispatches; the re-run may feed process() back in).
        std::vector<xi::Record> to_emit;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int n = (int)ring_.size();
            if (c == "replay_last") {
                int k = p["n"].as_int(1); if (k < 1) k = 1; if (k > n) k = n;
                for (int i = n - k; i < n; ++i) to_emit.push_back(ring_[(size_t)i].rec);
            } else if (c == "replay_all") {
                for (auto& e : ring_) to_emit.push_back(e.rec);
            } else if (c == "replay") {
                int i = p["index"].as_int(-1);
                if (i >= 0 && i < n) to_emit.push_back(ring_[(size_t)i].rec);
            } else if (c == "stop_replay") {
                // handled below (must not hold the lock while joining)
            } else if (c == "clear") {
                ring_.clear();
            } else if (c == "set_capacity") {
                int v = p["value"].as_int(capacity_); if (v < 1) v = 1;
                capacity_ = v;
                while ((int)ring_.size() > capacity_) ring_.pop_front();
            }
        }
        if (c == "stop_replay" || c == "clear") stop_replay_();
        for (auto& r : to_emit) {
            xi::Record copy = r;                          // keep the buffered one
            xi::emit_record(host_, name().c_str(), copy); // fresh id, ts = now
        }
        return get_def();
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return xi::Json::object()
            .set("capacity", capacity_)
            .set("count", (int)ring_.size())
            .set("replaying", replaying_.load())
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int v = p["capacity"].as_int(capacity_); if (v < 1) v = 1;
        std::lock_guard<std::mutex> lk(mu_);
        capacity_ = v;
        while ((int)ring_.size() > capacity_) ring_.pop_front();
        return true;
    }

private:
    struct Entry { xi::Record rec; int64_t t_us; };

    static int64_t mono_us_() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Stop any in-flight timed replay and join its thread. Safe to call from the
    // WS/exchange thread and the destructor; never called from the replay thread.
    void stop_replay_() {
        replay_stop_.store(true);
        if (replay_thread_.joinable()) replay_thread_.join();
        replay_stop_.store(false);
        replaying_.store(false);
    }

    void start_timed_(std::vector<Entry> snap, double speed) {
        stop_replay_();                       // cancel a prior replay first
        if (snap.empty()) return;
        replaying_.store(true);
        replay_thread_ = std::thread([this, snap = std::move(snap), speed]() {
            for (size_t i = 0; i < snap.size(); ++i) {
                if (replay_stop_.load()) break;
                if (i > 0) {
                    int64_t dt = snap[i].t_us - snap[i - 1].t_us;
                    if (dt < 0) dt = 0;
                    int64_t wait_us = (int64_t)((double)dt / speed);
                    // Interruptible sleep so stop_replay_/clear cancels promptly.
                    for (int64_t s = 0; s < wait_us && !replay_stop_.load(); ) {
                        int64_t chunk = std::min<int64_t>(wait_us - s, 20000);
                        std::this_thread::sleep_for(std::chrono::microseconds(chunk));
                        s += chunk;
                    }
                    if (replay_stop_.load()) break;
                }
                xi::Record copy = snap[i].rec;
                xi::emit_record(host_, name().c_str(), copy);
            }
            replaying_.store(false);
        });
    }

    mutable std::mutex     mu_;
    std::deque<Entry>      ring_;
    int                    capacity_ = 16;

    std::thread            replay_thread_;
    std::atomic<bool>      replay_stop_{false};
    std::atomic<bool>      replaying_{false};
};

XI_PLUGIN_IMPL(BufferReplay)
