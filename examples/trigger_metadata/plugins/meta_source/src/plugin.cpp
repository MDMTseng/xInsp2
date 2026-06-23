// meta_source.cpp — a source that tags every trigger with routing/context
// metadata (ABI v5).
//
// Instead of host->emit_trigger (frames only) it calls xi::emit_record, handing
// the bus a whole Record: a 16x16 frame PLUS a JSON metadata object
// ({command, recipe, seq}). The metadata rides the trigger bus BY POINTER
// (zero-serialize) and the inspect script reads it back with
// xi::current_trigger().meta() — no side-channel FIFO, no race.
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

class MetaSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    MetaSource(const xi_host_api* host, const char* name) : xi::Plugin(host, name) { start_(); }
    ~MetaSource() override { stop_(); }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return xi::Json::object()
            .set("period_ms", period_ms_.load())
            .set("recipe",    recipe_.load())
            .set("command",   command_)
            .set("emitted",   (int)(emitted_.load() & 0x7fffffff))
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int t = p["period_ms"].as_int(period_ms_.load()); if (t < 10) t = 10;
        period_ms_.store(t);
        recipe_.store(p["recipe"].as_int(recipe_.load()));
        auto c = p["command"].as_string();
        if (!c.empty()) { std::lock_guard<std::mutex> lk(mu_); command_ = c; }
        return true;
    }

private:
    mutable std::mutex    mu_;                 // guards command_
    std::atomic<int>      period_ms_{50};
    std::atomic<int>      recipe_{7};
    std::string           command_{"inspect_top"};
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::thread           thread_;

    void start_() { if (running_.load()) return; running_ = true; thread_ = std::thread([this] { run_loop_(); }); }
    void stop_()  { running_ = false; if (thread_.joinable()) thread_.join(); }

    void emit_one_() {
        const int W = 16, H = 16;
        uint64_t seq = seq_.fetch_add(1);
        int recipe;  std::string cmd;
        { std::lock_guard<std::mutex> lk(mu_); cmd = command_; recipe = recipe_.load(); }

        // 16x16 gray, seq stamped in the first 8 bytes (same convention as the
        // other sample sources).
        std::vector<uint8_t> px((size_t)W * H, 128);
        std::memcpy(px.data(), &seq, sizeof(seq));

        // The Record bundles the frame with the metadata. emit_record marshals
        // the image to a pool handle and hands the metadata doc over by pointer.
        xi::Record rec = xi::Record()
            .image("img", xi::Image(W, H, 1, px.data()))
            .set("command", cmd)
            .set("recipe",  recipe)
            .set("seq",     (int64_t)seq);

        xi_trigger_id tid; tid.hi = 0x6d657461736f7263ull; tid.lo = seq + 1;  // "metasorc"
        xi::emit_record(host_, name().c_str(), rec, tid);
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        using clk = std::chrono::steady_clock;
        while (running_.load()) {
            auto t0 = clk::now();
            emit_one_();
            auto next = t0 + std::chrono::milliseconds(period_ms_.load());
            while (running_.load() && clk::now() < next)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
};

XI_PLUGIN_IMPL(MetaSource)
