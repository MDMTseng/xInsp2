//
// trigger_source.cpp — minimal image source using the blessed emit() path.
//
// A source plugin PUSHES frames into the pipeline: build a record (one or
// more images + optional metadata) and hand it to the host with emit() — the
// xi::Plugin member that fills host()/name() and runs the RAII marshal/refcount
// path. The host dispatches the inspection script exactly once per emitted
// record; the script reads the frames back via xi::current_trigger().image(...).
//
// Each record carries a 128-bit trigger id. Pass XI_TRIGGER_NULL (the default)
// and the host mints a fresh one — that alone makes every frame individually
// addressable (its hex is current_trigger().id_string(), used by the
// buffer_replay plugin to replay a run). timestamp = 0 stamps host time.
//
// Correlating MULTIPLE sources "at the same event" (e.g. a hardware-synced
// stereo pair) is done by a GATHERING plugin that subscribes to the sources
// and emits ONE combined record — not by a bus policy (those were removed).
// See examples/stereo_sync/ for a paired-cameras reference.
//

#include <xi/xi_abi.hpp>    // xi::Plugin, xi::Record, xi::Image, pool_image()/emit()
#include <xi/xi_json.hpp>
#include <xi/xi_thread.hpp> // xi::spawn_worker — SEH-safe capture thread

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

class TriggerSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~TriggerSource() override { stop_(); }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
        else if (command == "set_fps")
            fps_ = std::max(1, std::min(120, p["value"].as_int(fps_)));
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running", running_.load())
            .set("fps",     fps_)
            .set("ticks",   (int)ticks_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        fps_ = p["fps"].as_int(fps_);
        return true;
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<int>  ticks_{0};
    int               fps_ = 10;
    std::thread       thread_;

    void start_() {
        if (running_.exchange(true)) return;
        // Blessed source thread: xi::spawn_worker installs the per-thread SEH
        // translator so a stray fault in run_loop_() is contained here instead
        // of taking down the whole backend (a raw std::thread would not be).
        thread_ = xi::spawn_worker(name() + "-source", [this] { run_loop_(); });
    }

    void stop_() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    void run_loop_() {
        const int W = 320, H = 240;
        int seq = 0;
        while (running_.load()) {
            // 1. Paint one frame straight into a fresh host-pool slot, so emit()
            //    hands it over via the pool refcount path (no heap-to-pool copy).
            xi::Image img = pool_image(W, H, 1);
            uint8_t* px = img.data();
            for (int i = 0; i < W * H; ++i) px[i] = (uint8_t)((i + seq) & 0xFF);

            // 2. Emit it. Default id = XI_TRIGGER_NULL → host mints a fresh
            //    trigger id; default ts = 0 → host stamps now(). The script
            //    reads this frame back as current_trigger().image("frame").
            xi::Record rec;
            rec.image("frame", img);
            emit(rec);

            ++seq;
            ticks_++;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / std::max(fps_, 1)));
        }
    }
};

XI_PLUGIN_IMPL(TriggerSource)
