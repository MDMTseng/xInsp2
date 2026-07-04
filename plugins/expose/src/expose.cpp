// expose.cpp — the official script data-out sink (the VAR replacement).
//
// A script surfaces output via xi::expose::send("lane", Record{values + images})
// (see <xi/xi_expose.hpp>). The channel id rides under the reserved key "$channel";
// the host stamps "$seq" (= wire run_id) for ordering. This sink:
//   - keeps the latest record per channel (raw values + owned images) for pull;
//   - for SUBSCRIBED channels only, JPEG-encodes each image through the HOST cache
//     (host->compress_image — encoded once globally) and PUSHES the whole record
//     as ONE atomic binary frame (host->emit_binary). No subscriber → no encode,
//     no push (subscription gating, tracked here in the plugin per the dumb-pipe
//     core: emit_binary is broadcast, the client filters by channel).
//
// FRAME FORMAT (plugin -> WS; clients + webui mirror this):
//   [0..3] magic 'XEX1' | [4..] a minimal msgpack map:
//     { v:1, channel:<str>, seq:<uint>, json:<str>, images:[ {key:<str>, jpeg:<bin>} ] }
//   `json` is the record's values serialized to a JSON string (display order =
//   key order); each image is JPEG (lossy, 8-bit). msgpack is hand-rolled below
//   (fixed shape, no dependency) but is valid msgpack wire format.
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_pack_contract.hpp>  // reserved keys: xi::pack_contract::kChannel/kSeq

#include "xex1_encode.hpp"    // the XEX1 msgpack encoder (shared with the fixture test)
#include "xex1_pack_dump.hpp"  // encode_pack_v3 — the generic pack->v3 walk (shared with record_save)
#include "xex1_wire_preview.hpp"  // encode_pack_v3_wire — expose's WS-SEND-only preview walk (E2)

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string b64(const uint8_t* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t b = p[i] << 16;
        if (i + 1 < n) b |= p[i + 1] << 8;
        if (i + 2 < n) b |= p[i + 2];
        o.push_back(T[(b >> 18) & 63]); o.push_back(T[(b >> 12) & 63]);
        o.push_back(i + 1 < n ? T[(b >> 6) & 63] : '=');
        o.push_back(i + 2 < n ? T[b & 63] : '=');
    }
    return o;
}

}  // namespace

class ExposeSink : public xi::Plugin {
public:
    // E2: resolve the xi.cap@1 consumer funnel once per instance (cached, and
    // re-resolve tolerant below). PRESENT + preview knob on => outgoing v3 WS
    // frames carry a full-resolution compressed `preview` entry INSTEAD of raw
    // pixels; ABSENT (or degraded) => today's raw path, untouched (fail-OPEN to
    // raw, never to nothing).
    ExposeSink(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        if (host && host->get_interface)
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
    }

    // polaris2 wave-2 (docs/new_gen/08 Wave 2 step 3): the xi.pack@1 pack-in/
    // pack-out door — THE data plane (v12: the Record process path is deleted).
    // expose is a SINK — it consumes a sealed pack and its real output is the
    // emit_binary side-effect; this door returns a small ack pack {channel, seen}.
    // It walks the input pack GENERICALLY (count()+key_at()+tag_at()+typed reads
    // — zero producer knowledge; the r2 constraint made real).
    //
    // The framework-internal reserved keys ($channel/$seq) are lifted to the wire
    // frame's top-level fields; every OTHER entry is dumped by tag: scalars/str
    // pass through, image entries get the JPEG preview treatment (v1) or are
    // inlined as raw bin (v3), and nested msgpack rides verbatim (v3). Which wire
    // version is produced is the `frame_wire_v3` config. [v12 THE CUT: the DEFAULT
    // flipped to v3 (the durable tagged-msgpack wire). XEX1-v1 encode stays
    // selectable via frame_wire_v3:false for ONE release, then is deleted —
    // doc 06 §6.2, app-team accepted.]
    void process(xi::PackIn& in, xi::PackOut& out) override {
        const std::string channel(in.str(xi::pack_contract::kChannel).value_or("default"));
        const uint64_t    seq = (uint64_t)in.i64_or(xi::pack_contract::kSeq, 0);

        bool v3;
        { std::lock_guard<std::mutex> lk(mu_); v3 = wire_v3_; }

        // Walk the borrowed input pack WITHOUT our mutex (it is immutable +
        // host-owned); take the lock only to publish the result + read state.
        // The v3 path routes through the WS-wire builder, which substitutes a
        // compressed preview for raw pixels when the imgcodec capability is live
        // (E2); it falls open to the SHARED raw encode_pack_v3 otherwise.
        std::vector<uint8_t> frame = v3 ? build_wire_v3_from_pack_(in, channel, seq)
                                        : build_v1_from_pack_(in, channel, seq);

        long long seen; bool subscribed;
        {
            std::lock_guard<std::mutex> lk(mu_);
            Channel& ch = channels_[channel];
            ch.seq         = seq;
            ch.frame_bytes = frame;    // keep the latest encoded frame for pull
            seen = ++ch.seen;
            subscribed = subscribed_.count(channel) != 0;
        }
        if (subscribed) emit_binary(frame);

        out.str("channel", channel).i64("seen", (int64_t)seen);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string c = p["command"].as_string();
        std::lock_guard<std::mutex> lk(mu_);

        if (c == "subscribe" || c == "unsubscribe") {
            const bool sub = (c == "subscribe");
            p["channels"].for_each([&](const char*, const xi::Json& v) {
                const std::string name = v.as_string();
                if (name.empty()) return;
                if (sub) subscribed_.insert(name); else subscribed_.erase(name);
            });
            auto arr = xi::Json::array();
            for (auto& s : subscribed_) arr.push(s);
            return xi::Json::object().set("ok", true).set("subscribed", arr).dump();
        }
        if (c == "list_channels") {
            auto chans = xi::Json::object();
            for (auto& [name, ch] : channels_)
                chans.set(name.c_str(), xi::Json::object()
                    .set("seen", (int64_t)ch.seen)
                    .set("subscribed", subscribed_.count(name) != 0));
            return xi::Json::object().set("count", (int)channels_.size())
                .set("channels", chans).dump();
        }
        if (c == "get" || c == "get_latest") {  // pull latest: the SAME frame, base64'd
            const std::string channel = p["channel"].as_string("default");
            auto it = channels_.find(channel);
            if (it == channels_.end())
                return xi::Json::object().set("found", false).set("channel", channel).dump();
            // A channel fed through the pack door holds its encoded XEX1 frame
            // bytes — the sole data plane in v12 (the Record rebuild is gone).
            const std::vector<uint8_t>& frame = it->second.frame_bytes;
            return xi::Json::object().set("found", true).set("channel", channel)
                .set("seq", (int64_t)it->second.seq)
                .set("frame_b64", b64(frame.data(), frame.size())).dump();
        }
        if (c == "clear") channels_.clear();
        return xi::Json::object().set("count", (int)channels_.size()).dump();
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        auto names = xi::Json::array();
        for (auto& [name, ch] : channels_) { (void)ch; names.push(name); }
        return xi::Json::object().set("count", (int)channels_.size())
            .set("channels", names)
            .set("frame_wire_v3", wire_v3_)
            // E2 preview knobs + live state (read-only introspection).
            .set("preview_compress", preview_compress_)
            .set("preview_quality", (int)preview_quality_)
            .set("preview_degraded", degraded_).dump();
    }
    // Config: opt the pack-in door into the canonical XEX1-v3 wire dump
    // (default off — v1 stays the default wire per the plan's governance). The
    // Record path is unaffected; this flag only picks the pack-door encoder.
    // E2: `preview_compress` (default ON) gates the compressed-preview
    // substitution on the v3 WS-send path; `preview_quality` (1..100) sets the
    // JPEG quality. Turning the knob OFF (or having no imgcodec provider) leaves
    // the raw v3 wire exactly as before.
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return true;   // tolerant: absent/blank def is a no-op
        std::lock_guard<std::mutex> lk(mu_);
        if (p["frame_wire_v3"].valid()) wire_v3_ = p["frame_wire_v3"].as_bool(wire_v3_);
        if (p["preview_compress"].valid())
            preview_compress_ = p["preview_compress"].as_bool(preview_compress_);
        if (p["preview_quality"].valid()) {
            int q = (int)p["preview_quality"].as_double(preview_quality_);
            preview_quality_ = q < 1 ? 1 : (q > 100 ? 100 : q);
        }
        return true;
    }

private:
    struct Channel {
        uint64_t                          seq  = 0;
        long long                         seen = 0;
        std::vector<uint8_t>              frame_bytes;  // latest encoded frame (pack-door path)
    };

    // --- pack-door encoders (the generic walk; docs/new_gen/08 Wave 2) --------

    // XEX1-v3 RAW: the canonical frame dump via the ONE shared implementation in
    // xex1_pack_dump.hpp — so expose's raw wire frame and record_save's on-disk
    // file are BYTE-IDENTICAL for the same pack (doc 10 gate P3). record_save
    // ALWAYS emits this; expose emits it when preview is off / unavailable /
    // degraded (fail-open to raw).
    std::vector<uint8_t> build_v3_from_pack_(const xi::PackIn& in,
                                              const std::string& channel,
                                              uint64_t seq) const {
        return xi::xex1::encode_pack_v3(in, channel, seq);
    }

    // E2 — the WS-SEND v3 builder. When the imgcodec capability is live and the
    // preview knob is on, each IMAGE entry carries a full-resolution compressed
    // `preview` beside its (now-empty) raw px (xex1_wire_preview.hpp). This is
    // the ONLY place the substitution happens; record_save's shared raw dump is
    // never touched, so the record->replay byte identity is preserved.
    //
    // Availability + degraded mode (contract fact 3): the imgcodec provider ships
    // on_fault:"refuse", so ONE hard fault quarantines it and every later cap
    // call returns -3 forever. So a codec-wide failure flips this instance into a
    // PERSISTENT degraded state (raw + one status line + one log per transition)
    // and stops calling the cap every frame; it re-probes only occasionally
    // (throttled) via the cheap available() gate + a spaced retry. A per-image
    // contract $fault (e.g. a wrong-channel image) is NOT codec-wide: that image
    // alone falls to raw, the codec stays healthy.
    std::vector<uint8_t> build_wire_v3_from_pack_(const xi::PackIn& in,
                                                  const std::string& channel,
                                                  uint64_t seq) {
        bool try_preview;
        int  quality;
        const xi_cap_v1* cap;
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Re-resolve tolerant: a host that gained the cap plane after our
            // construction still gets picked up (cheap, process-stable pointer).
            if (!cap_ && host() && host()->get_interface)
                cap_ = static_cast<const xi_cap_v1*>(host()->get_interface("xi.cap", 1));
            cap     = cap_;
            quality = preview_quality_;

            const bool avail =
                preview_compress_ && cap && cap->available("xi.jpeg.encode");
            if (!preview_compress_) {
                try_preview = false;                       // knob off -> raw
            } else if (!avail) {
                // No provider registered -> persistent raw (the fallback leg).
                set_degraded_locked_(true, "no xi.jpeg.encode provider");
                try_preview = false;
            } else if (degraded_) {
                // Codec registered but we're degraded: retry only every N frames
                // (no per-frame fault storm against a quarantined provider).
                try_preview = (++reprobe_ctr_ >= kReprobeEvery);
                if (try_preview) reprobe_ctr_ = 0;
            } else {
                try_preview = true;                        // healthy
            }
        }

        if (!try_preview)
            return build_v3_from_pack_(in, channel, seq);  // shared raw dump

        // The host xi.pack@1 builder for the encode-request packs (cached).
        const xi_pack_v1* pk = pack_iface();
        bool codec_down = false, raw_fallback = false;
        std::vector<uint8_t> frame = xi::xex1::encode_pack_v3_wire(
            in, channel, seq,
            [pk, cap, quality](const uint8_t* px, int w, int h, int c, int& q,
                               std::vector<uint8_t>& jpeg) {
                return encode_one_(pk, cap, quality, px, w, h, c, q, jpeg);
            },
            &codec_down, &raw_fallback);

        {
            std::lock_guard<std::mutex> lk(mu_);
            if (codec_down)      set_degraded_locked_(true,  "codec faulted");
            else if (degraded_)  set_degraded_locked_(false, "codec recovered");
        }
        (void)raw_fallback;   // per-image contract faults are benign (raw leg)
        return frame;
    }

    // Flip degraded state; emit exactly ONE status line + ONE stderr log per
    // transition (never per frame). Caller holds mu_.
    void set_degraded_locked_(bool degraded, const char* reason) {
        if (degraded == degraded_) return;
        degraded_ = degraded;
        reprobe_ctr_ = 0;
        if (degraded) {
            status(std::string("expose: jpeg preview OFF (raw fallback) - ") + reason);
            std::fprintf(stderr,
                "[expose:%s] preview degraded -> raw (%s)\n", name().c_str(), reason);
        } else {
            status("expose: jpeg preview ON");
            std::fprintf(stderr,
                "[expose:%s] preview recovered (%s)\n", name().c_str(), reason);
        }
    }

    // Encode ONE image through xi.jpeg.encode. Returns the fail-open verdict the
    // wire walk acts on. Triggers a RAW fallback on ALL THREE documented signals
    // (contract fact 2): negative funnel rc (-> CodecDown, a codec-wide signal),
    // a `$fault` in the (rc==0) result pack, or a zero-length jpeg bin.
    static xi::xex1::PreviewOutcome encode_one_(
            const xi_pack_v1* pk, const xi_cap_v1* cap, int quality,
            const uint8_t* px, int w, int h, int c,
            int& q_out, std::vector<uint8_t>& jpeg_out) {
        using xi::xex1::PreviewOutcome;
        if (!pk || !cap) return PreviewOutcome::CodecDown;
        q_out = quality;
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_image(b, "image", w, h, c, px);
        pk->builder_add_i64(b, "quality", (int64_t)quality);
        pk->builder_add_i64(b, "$v", 1);
        xi_pack_handle req = pk->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        const int32_t rc = cap->call("xi.jpeg.encode", req, &rsp);
        pk->release(req);
        if (rc < 0) {                       // -1/-2/-3/-4/-5: codec-wide down
            if (rsp != XI_PACK_NULL) pk->release(rsp);
            return PreviewOutcome::CodecDown;
        }
        if (rsp == XI_PACK_NULL)            // rc 0 but no answer -> bad input
            return PreviewOutcome::RawFallback;
        const char* fp = nullptr; int32_t fn = 0;
        const bool faulted = pk->get_str(rsp, "$fault", &fp, &fn) && fp;   // contract fault
        const void* jp = nullptr; int32_t jn = 0;
        const bool has_jpeg = pk->get_bin(rsp, "jpeg", &jp, &jn) && jp && jn > 0;
        if (faulted || !has_jpeg) {
            pk->release(rsp);
            return PreviewOutcome::RawFallback;
        }
        jpeg_out.assign(static_cast<const uint8_t*>(jp),
                        static_cast<const uint8_t*>(jp) + jn);
        pk->release(rsp);
        return PreviewOutcome::Compressed;
    }

    // XEX1-v1 from a pack: the legacy display shape (same bytes existing clients
    // decode). Scalar/str entries collect into the json values object; image
    // entries get the JPEG preview treatment (host cache). bin/mp entries have no
    // v1 (display) representation and are v3-only — dropped here.
    std::vector<uint8_t> build_v1_from_pack_(const xi::PackIn& in,
                                              const std::string& channel,
                                              uint64_t seq) const {
        auto vals = xi::Json::object();
        std::vector<xi::xex1::EncImage> images;
        const int n = in.count();
        for (int i = 0; i < n; ++i) {
            auto keyv = in.key_at(i);
            if (!keyv) continue;
            std::string key(*keyv);
            if (key == xi::pack_contract::kChannel || key == xi::pack_contract::kSeq) continue;
            switch (in.tag_at(i)) {
                case XI_PACK_TAG_I64:
                    vals.set(key.c_str(), (int64_t)in.i64_or(key.c_str(), 0)); break;
                case XI_PACK_TAG_F64:
                    vals.set(key.c_str(), in.f64(key.c_str()).value_or(0.0)); break;
                case XI_PACK_TAG_BOOL:
                    vals.set(key.c_str(), in.boolean(key.c_str()).value_or(false)); break;
                case XI_PACK_TAG_STR:
                    vals.set(key.c_str(), std::string(in.str(key.c_str()).value_or(""))); break;
                case XI_PACK_TAG_IMAGE: {
                    auto im = in.image(key.c_str());
                    if (!im || !im->pixels) break;
                    std::vector<uint8_t> jpeg = compress_px_(
                        static_cast<const uint8_t*>(im->pixels), im->width, im->height, im->channels);
                    if (!jpeg.empty()) images.push_back({key, std::move(jpeg)});
                    break;
                }
                default: break;  // bin/mp: no v1 display form
            }
        }
        return xi::xex1::encode_frame(channel, seq, vals.dump(), images);
    }

    // JPEG-encode via the host cache, through the SDK xi::Plugin::compress()
    // wrapper: identical images are encoded once globally, and we don't link a
    // codec ourselves. compress() resolves the frozen xi.preview@1 interface
    // (ABI v10 capability segregation) once and caches it, falling back to the
    // legacy host->compress_image field on a pre-v10 host — same host encoder,
    // same bytes, same -needed/0 return convention.
    // JPEG-through-host-cache path, from raw pixel bytes (the pack-door
    // walk holds a borrowed pool span, not an xi::Image).
    std::vector<uint8_t> compress_px_(const uint8_t* px, int w, int h, int c) const {
        if (!px || w <= 0 || h <= 0 || c <= 0) return {};
        std::vector<uint8_t> jpeg(64 * 1024);
        int n = compress(px, w, h, c, 85, jpeg.data(), (int)jpeg.size());
        if (n < 0) {  // buffer too small — resize to needed and retry
            jpeg.resize((size_t)(-n));
            n = compress(px, w, h, c, 85, jpeg.data(), (int)jpeg.size());
        }
        if (n <= 0) return {};
        jpeg.resize((size_t)n);
        return jpeg;
    }

    mutable std::mutex              mu_;
    std::map<std::string, Channel>  channels_;
    std::set<std::string>           subscribed_;
    bool                            wire_v3_ = true;    // v12 default: XEX1-v3 (v1 opt-out for one release)

    // --- E2 preview state (all guarded by mu_) --------------------------------
    const xi_cap_v1*  cap_ = nullptr;      // xi.jpeg.encode funnel (resolved in ctor)
    bool              preview_compress_ = true;   // knob: substitute compressed preview (default ON)
    int               preview_quality_  = 85;     // JPEG quality for previews
    bool              degraded_ = false;   // persistent raw-fallback mode (codec down/absent)
    int               reprobe_ctr_ = 0;    // throttle for re-probing a degraded codec
    static constexpr int kReprobeEvery = 30;  // frames between degraded re-probes
};

XI_PLUGIN_IMPL(ExposeSink)
// polaris2 wave-2: publish the xi.pack@1 pack-in/pack-out door so the host
// learns expose consumes packs (the generic sink — it walks any sealed pack
// without producer knowledge). The Record process() path is untouched.
XI_PLUGIN_PACK_DOOR(ExposeSink)
