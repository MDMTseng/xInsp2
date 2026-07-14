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

#include <xi/xi_abi.hpp>    // xi::Plugin, xi::PackOut, xi::Image, new_pack()/emit()
#include <xi/xi_mp.hpp>     // canonical msgpack Writer — the xi/image blob descriptor
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

    // Zero-copy xi/image emit (spec 30): mint a self-describing blob, paint into
    // its 64B-aligned payload IN PLACE (via a non-owning Image view), adopt it
    // (the pack addrefs), then drop our mint ref — no heap-to-pool copy. The
    // successor to the old adopt_image pool-handle hand-off.
    template <class Paint>
    bool add_image_blob_(xi::PackOut& f, const char* key,
                         int w, int h, int c, Paint&& paint) {
        xi::mp::Writer dw;                    // {"t":"xi/image","w","h","c","dt"}
        dw.map(5);
        dw.key("t");  dw.str("xi/image");
        dw.key("w");  dw.int_(w);
        dw.key("h");  dw.int_(h);
        dw.key("c");  dw.int_(c);
        dw.key("dt"); dw.str("u8");
        void* pp = nullptr;
        xi_image_handle bh = f.blob_mint(dw.bytes().data(), (int32_t)dw.bytes().size(),
                                         (int64_t)w * h * c, &pp);
        if (!bh || !pp) return false;
        xi::Image v = xi::Image::view(w, h, c, static_cast<const uint8_t*>(pp));
        paint(v);
        bool ok = f.adopt_blob(key, bh);
        host_->image_release(bh);
        return ok;
    }

    void run_loop_() {
        const int W = 320, H = 240;
        int seq = 0;
        while (running_.load()) {
            // Emit one frame as a self-describing xi/image blob, painted IN PLACE
            // in the minted buffer's 64B-aligned payload (zero-copy). new_pack()
            // starts a host-side builder carrying the @4 blob supplement; emit()
            // seals + dispatches. Default id = XI_TRIGGER_NULL → host mints a
            // fresh trigger id; default ts = 0 → host stamps now(). The script
            // reads this frame back as current_trigger().pack().get_image("frame")
            // (the @1 xi/image blob adapter).
            xi::PackOut f = new_pack();
            f.i64("seq", seq);
            add_image_blob_(f, "frame", W, H, 1, [&](xi::Image& img) {
                uint8_t* px = img.data();
                for (int i = 0; i < W * H; ++i) px[i] = (uint8_t)((i + seq) & 0xFF);
            });
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
