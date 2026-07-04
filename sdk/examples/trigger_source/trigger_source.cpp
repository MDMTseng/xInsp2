//
// trigger_source.cpp — minimal image source using the blessed emit() path.
//
// A source plugin PUSHES frames into the pipeline: build a sealed xi.pack@1
// pack (one or more image entries + optional metadata) and hand it to the
// host with emit() — the xi::Plugin member that seals the pack, dispatches
// it, and drops our ref (one call owns the whole builder_seal / emit_pack /
// release refcount dance). The host dispatches the inspection script exactly
// once per emitted pack; the script reads the frame back via
//     auto t = xi::current_trigger();
//     if (auto f = t.pack()) { auto img = f.get_image("frame"); ... }
//
// Each emitted pack carries a 128-bit trigger id. Pass XI_TRIGGER_NULL (the
// default) and the host mints a fresh one — that alone makes every frame
// individually addressable (its hex is current_trigger().id_string(), used by
// the buffer_replay plugin to replay a run). timestamp = 0 stamps host time.
//
// Correlating MULTIPLE sources "at the same event" (e.g. a hardware-synced
// stereo pair) is done by a GATHERING plugin that subscribes to the sources
// and emits ONE combined pack — not by a bus policy (those were removed).
// See examples/stereo_sync/ for a paired-cameras reference.
//
// A PURE source like this one has no per-call data plane, so it neither
// overrides process(PackIn&, PackOut&) nor publishes XI_PLUGIN_PACK_DOOR. A
// source that ALSO wants an in-band control door (closed-loop actuation from
// a script) adds both on top — see plugins/mock_camera for that pattern.
//

#include <xi/xi_abi.hpp>    // xi::Plugin, xi::PackOut, xi::Image, pool_image()/new_pack()/emit()
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
            // 1. Paint one frame straight into a fresh host-pool slot, so the
            //    pack can adopt it by refcount (no heap-to-pool copy).
            xi::Image img = pool_image(W, H, 1);
            uint8_t* px = img.write();   // blessed writable-output accessor
            for (int i = 0; i < W * H; ++i) px[i] = (uint8_t)((i + seq) & 0xFF);

            // 2. Emit it. new_pack() starts a host-side builder; adopt_image
            //    hands the pool slot to the pack by refcount (the pack addrefs,
            //    `img` keeps its own ref); emit() seals + dispatches. Default
            //    id = XI_TRIGGER_NULL → host mints a fresh trigger id; default
            //    ts = 0 → host stamps now(). The script reads this frame back
            //    as current_trigger().pack().get_image("frame").
            xi::PackOut f = new_pack();
            f.i64("seq", seq);
            f.adopt_image("frame", W, H, 1, img.pool_handle());
            emit(std::move(f));

            ++seq;
            ticks_++;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(1000 / std::max(fps_, 1)));
        }
    }
};

// A pure source publishes no pack door (nothing to call in-band) — just the
// standard C-ABI exports. See the note at the top.
XI_PLUGIN_IMPL(TriggerSource)
