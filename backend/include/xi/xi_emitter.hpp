#pragma once
//
// xi_emitter.hpp — the emitter helper for the pull-by-id dispatch model. Wraps
// the 5-step boilerplate a source plugin would otherwise hand-roll (mint res_id,
// assign a contiguous seq, stage images + metadata, emit_resource, emit_dispatch)
// behind a small stateful object, and gets the seq-contiguity contract right so
// authors don't have to.
//
// Usage (a source plugin holds one as a member and binds it once host() is up):
//
//   class Cam : public xi::Plugin {
//     xi::Emitter em_;
//     void grab_loop() {
//       em_.bind(host(), name());
//       for (;;) {
//         xi::Image frame = capture();
//         em_.image("img", frame);
//         if (!em_.emit())  { /* back-pressure: lane full — this frame was
//                                dropped at the source, its seq NOT consumed,
//                                so the downstream seq stream stays gap-free */ }
//       }
//     }
//   };
//
// Identity vs order: res_id (the pull key) is the seq's hex — opaque and unique
// per live frame. Ordering for downstream serialization (comm) rides as the
// "seq" field auto-injected into each frame's dataInfo. A rejected emit reuses
// the same res_id+seq next time (overwriting the orphaned resource in the ring),
// so seq is contiguous by construction and nothing leaks.
//
#include "xi_abi.h"      // xi_host_api, xi_record_image, xi_trigger_id
#include "xi_image.hpp"  // Image (pool_handle)

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace xi {

class Emitter {
public:
    Emitter() = default;
    Emitter(const xi_host_api* host, std::string name)
        : host_(host), name_(std::move(name)) {}

    // Bind to the host + emitter name once they're available (idempotent — a
    // plugin can call this at the top of every entry point). Does not reset seq.
    void bind(const xi_host_api* host, const std::string& name) {
        host_ = host;
        if (name_.empty()) name_ = name;
    }

    // Stage a named image for the next frame (zero-copy). The Emitter ADDREFS
    // the handle and holds that ref until emit(), so the caller may pass a
    // temporary (e.g. image("img", pool_image(...))) — its own ref can drop
    // immediately without freeing the handle out from under us. Chainable.
    // Requires bind()/ctor host first.
    Emitter& image(const std::string& key, const Image& img) {
        xi_image_handle h = img.pool_handle();
        if (h != XI_IMAGE_NULL && host_ && host_->image_addref) {
            host_->image_addref(h);
            staged_.emplace_back(key, h);
        }
        return *this;
    }

    // Emit the staged frame: stage it under res_id = hex(seq) with metadata
    // {"seq":N, ...extra}, then dispatch a run for it. `extra` is an optional
    // JSON object (e.g. R"({"recipe":"A"})") merged alongside the seq.
    //
    // Returns true if the run was accepted (seq consumed, ++). Returns false on
    // back-pressure (lane full): the seq is NOT consumed and the staged frame is
    // cleared — so the dropped frame never enters the seq stream (no gap), and
    // the next emit reuses the same res_id, overwriting the orphan.
    bool emit(const std::string& extra = "") {
        if (!host_ || !host_->emit_resource || !host_->emit_dispatch) {
            release_staged_();
            return false;
        }
        xi_trigger_id tid{0, seq_};
        char idbuf[40];
        std::snprintf(idbuf, sizeof(idbuf), "%016llx%016llx",
                      (unsigned long long)tid.hi, (unsigned long long)tid.lo);

        std::string meta = "{\"seq\":" + std::to_string((unsigned long long)seq_);
        if (extra.size() >= 2 && extra.front() == '{' && extra.back() == '}' && extra != "{}")
            meta += "," + extra.substr(1);     // splice extra's fields, keep its closing }
        else
            meta += "}";

        std::vector<xi_record_image> imgs;
        imgs.reserve(staged_.size());
        for (auto& [k, h] : staged_) imgs.push_back({k.c_str(), h});

        host_->emit_resource(name_.c_str(), idbuf, imgs.data(), (int)imgs.size(), meta.c_str());
        int ok = host_->emit_dispatch(name_.c_str(), tid, 0);
        release_staged_();   // drop the Emitter's refs; the store kept its own
        if (ok) { ++seq_; return true; }
        return false;   // rejected: seq kept; the staged resource will be overwritten next emit
    }

    uint64_t           seq()  const { return seq_; }   // next seq to be assigned
    const std::string& name() const { return name_; }

private:
    void release_staged_() {
        if (host_ && host_->image_release)
            for (auto& [k, h] : staged_) host_->image_release(h);
        staged_.clear();
    }
    const xi_host_api* host_ = nullptr;
    std::string        name_;
    uint64_t           seq_  = 0;     // contiguous; burned only on a successful emit
    std::vector<std::pair<std::string, xi_image_handle>> staged_;
};

} // namespace xi
