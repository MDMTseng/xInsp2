//
// synced_stereo.cpp — synthetic stereo camera: a GATHERING source that grabs
// both cameras and emits left+right in ONE event. Multi-camera sync needs no
// bus policy — the frames are correlated because they ride the same container.
//
// Per tick:
//   1. Build two distinct frames (left = vertical stripes, right = horizontal)
//      stamped with the same `seq` so the script can verify they really come
//      from the same event.
//   2. Gather them into ONE sealed Pack — the left + right image entries plus
//      the seq entry — and emit it under a single trigger. v12 (THE CUT) + spec
//      30: the sealed Pack is the SOLE data plane; each image is a
//      self-describing xi/image BLOB minted and painted IN PLACE (zero-copy
//      adopt_blob addref), and a host without the blob plane cannot run this
//      source's emit path.
//
// The dispatched event carries both images; a script reads them via the pack
// (t.pack().image("left") / .image("right"), the @1 xi/image blob adapter) or
// the expose walk.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_mp.hpp>         // canonical msgpack Writer — build the xi/image blob descriptor
#include <xi/xi_json.hpp>
#include <xi/xi_contract.hpp>   // fail-loud schema-skew guard (kSchemaKey)
#include <xi/xi_thread.hpp>   // xi::spawn_worker — SEH-safe capture thread

#include <utility>

#include "synced_stereo_keys.gen.h"   // the ONE key contract (kLeft/kRight/kSeq)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace keys = xi::synced_stereo::keys;

class SyncedStereo : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    ~SyncedStereo() override { stop_(); }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if      (command == "start") start_();
        else if (command == "stop")  stop_();
        else if (command == "fire") {                 // deterministic headless drive
            int n = p["n"].as_int(1); if (n < 1) n = 1; if (n > 10000) n = 10000;
            for (int i = 0; i < n; ++i) emit_one_();
        }
        else if (command == "set_fps") {
            int v = p["value"].as_int(fps_.load());
            if (v < 1) v = 1;
            if (v > 120) v = 120;
            fps_ = v;
        }
        return get_def();
    }

    std::string get_def() const override {
        return xi::Json::object()
            .set("running", running_.load())
            .set("fps", fps_.load())
            .set("ticks", (int)ticks_.load())
            .dump();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        // Guard 3: reject a config built against an incompatible header schema
        // with a precise error naming both versions (mirrors mock_camera /
        // json_source). Absent stamp (a legacy persisted instance.json) is
        // tolerated.
        auto sv = p[xi::contract::kSchemaKey];
        if (sv.is_number() && sv.as_int() != xi::synced_stereo::kSchemaVersion) {
            log_error("synced_stereo: config schema mismatch: built for v" +
                      std::to_string(sv.as_int()) + ", this plugin serves v" +
                      std::to_string(xi::synced_stereo::kSchemaVersion));
            return false;
        }
        fps_ = p["fps"].as_int(fps_.load());
        return true;
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<int>  ticks_{0};
    // Written from the control thread (exchange/set_def), read by run_loop_()'s
    // worker — atomic for the same reason mock_camera's config fields are.
    std::atomic<int>  fps_{10};
    std::thread       thread_;

    void start_() {
        if (running_.load()) return;
        running_ = true;
        // Blessed source thread: xi::spawn_worker installs the per-thread SEH
        // translator + top-level catch so a stray fault in the grab loop is
        // contained to this worker instead of taking down the whole backend.
        thread_ = xi::spawn_worker(name() + "-source", [this] { run_loop_(); });
    }

    void stop_() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    std::atomic<int> seq_{0};

    // Zero-copy xi/image emit (spec 30): mint a self-describing blob, paint into
    // its 64B-aligned payload IN PLACE (via a non-owning Image view), adopt it
    // into the pack (which addrefs), then drop our mint ref — no per-frame image
    // memcpy. The successor to the old adopt_image pool-handle hand-off (that @1
    // door adapter now copies raw pixels into a headed blob).
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

    void emit_one_() {
        const int W = 320, H = 240;
        int seq = seq_.fetch_add(1);

        // v12 + spec 30: gather the correlated pair into ONE sealed Pack — both
        // images (as self-describing xi/image BLOBS, painted IN PLACE in the
        // minted buffers, zero-copy) plus the seq entry under a single trigger.
        // LEFT: vertical stripes. RIGHT: horizontal stripes. Same seq. The
        // sealed pack is the sole data plane; a host without the blob plane
        // can't run this source's emit path.
        xi::PackOut f = new_pack();
        f.i64(keys::kSeq, seq);
        add_image_blob_(f, keys::kLeft, W, H, 1, [&](xi::Image& img) {
            uint8_t* lp = img.data();
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    lp[y * W + x] = (uint8_t)(((x + seq) & 31) ? 200 : 32);
            std::memcpy(lp, &seq, sizeof(seq));   // stamp seq: same-event check
        });
        add_image_blob_(f, keys::kRight, W, H, 1, [&](xi::Image& img) {
            uint8_t* rp = img.data();
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    rp[y * W + x] = (uint8_t)(((y + seq) & 31) ? 32 : 200);
            std::memcpy(rp, &seq, sizeof(seq));
        });
        emit(std::move(f));
        ticks_++;
    }

    void run_loop_() {
        while (running_.load()) {
            emit_one_();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000 / std::max(fps_.load(), 1)));
        }
    }
};

XI_PLUGIN_IMPL(SyncedStereo)
