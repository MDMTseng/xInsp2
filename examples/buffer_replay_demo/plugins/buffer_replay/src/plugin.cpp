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

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <deque>
#include <mutex>
#include <string>
#include <vector>

class BufferReplay : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Pass-through + capture. Buffering keeps frames ACROSS calls, but an input
    // image is only valid for THIS call (the host owns its handle for the
    // duration of process). So we OWN the pixels: deep-copy each image into a
    // heap-backed Image. The metadata doc rides the Record copy.
    xi::Record process(const xi::Record& in) override {
        xi::Record owned = in;
        for (auto& [k, img] : in.images())
            if (!img.empty())
                owned.image(k, xi::Image(img.width, img.height, img.channels, img.data()));

        std::lock_guard<std::mutex> lk(mu_);
        ring_.push_back(std::move(owned));
        while ((int)ring_.size() > capacity_) ring_.pop_front();
        return xi::Record().set("buffered", (int64_t)ring_.size());
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto c = p["command"].as_string();
        // Snapshot the records to replay UNDER the lock, then emit OUTSIDE it
        // (emit_record dispatches; the re-run may feed process() back in).
        std::vector<xi::Record> to_emit;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int n = (int)ring_.size();
            if (c == "replay_last") {
                int k = p["n"].as_int(1); if (k < 1) k = 1; if (k > n) k = n;
                for (int i = n - k; i < n; ++i) to_emit.push_back(ring_[(size_t)i]);
            } else if (c == "replay_all") {
                for (auto& r : ring_) to_emit.push_back(r);
            } else if (c == "replay") {
                int i = p["index"].as_int(-1);
                if (i >= 0 && i < n) to_emit.push_back(ring_[(size_t)i]);
            } else if (c == "clear") {
                ring_.clear();
            } else if (c == "set_capacity") {
                int v = p["value"].as_int(capacity_); if (v < 1) v = 1;
                capacity_ = v;
                while ((int)ring_.size() > capacity_) ring_.pop_front();
            }
        }
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
    mutable std::mutex     mu_;
    std::deque<xi::Record> ring_;
    int                    capacity_ = 16;
};

XI_PLUGIN_IMPL(BufferReplay)
