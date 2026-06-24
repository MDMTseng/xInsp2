// pulse_src.cpp — a trivial source for the buffer_replay demo: each tick emits a
// 16x16 gray frame whose fill value rises with the seq, so a threshold-based
// inspect gives a frame-dependent result. start/stop + fire exchange.
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

class PulseSrc : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    PulseSrc(const xi_host_api* host, const char* name) : xi::Plugin(host, name) { start_(); }
    ~PulseSrc() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object().set("period_ms", period_ms_.load())
                                 .set("seq", (int)seq_.load()).dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        int t = p["period_ms"].as_int(period_ms_.load()); if (t < 10) t = 10;
        period_ms_.store(t);
        return true;
    }
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto c = p["command"].as_string();
        if (c == "start") start_();
        else if (c == "stop") stop_();
        else if (c == "fire") { int n = p["n"].as_int(1); if (n < 1) n = 1; if (n > 1000) n = 1000;
                                for (int i = 0; i < n; ++i) emit_one_(); }
        return get_def();
    }

private:
    std::atomic<bool>     running_{false};
    std::atomic<int>      period_ms_{50};
    std::atomic<uint32_t> seq_{0};
    std::thread           thread_;

    void start_() { if (running_.load()) return; running_ = true; thread_ = std::thread([this]{ run_loop_(); }); }
    void stop_()  { running_ = false; if (thread_.joinable()) thread_.join(); }

    void emit_one_() {
        const int W = 16, H = 16;
        uint32_t seq = seq_.fetch_add(1);
        uint8_t fill = (uint8_t)((seq * 17) & 0xFF);     // frame-dependent brightness
        std::vector<uint8_t> px((size_t)W * H, fill);
        std::memcpy(px.data(), &seq, sizeof(seq));
        xi::Record rec = xi::Record().image("img", xi::Image(W, H, 1, px.data()))
                                     .set("seq", (int64_t)seq);
        xi::emit_record(host_, name().c_str(), rec);     // auto id, ts = now
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

XI_PLUGIN_IMPL(PulseSrc)
