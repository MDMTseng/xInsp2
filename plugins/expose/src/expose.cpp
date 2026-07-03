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

#include "xex1_encode.hpp"    // the XEX1 msgpack encoder (shared with the fixture test)
#include "xex1_pack_dump.hpp"  // encode_pack_v3 — the generic pack->v3 walk (shared with record_save)

#include <cstdint>
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
    using xi::Plugin::Plugin;

    // Reserved keys — sourced from the central registry (xi::Record) so this
    // sink can't drift from the framework's staged-emit contract.
    static constexpr const char* kChannelKey = xi::Record::kChannelKey;
    static constexpr const char* kSeqKey     = xi::Record::kSeqKey;

    xi::Record process(const xi::Record& in) override {
        xi::Json j = xi::Json::parse(in.data_json());
        const std::string channel = j[kChannelKey].as_string("default");
        const uint64_t    seq     = (uint64_t)(int64_t)j[kSeqKey].as_double(0);
        // The frame's values = the record minus the framework-internal reserved keys
        // (channel + seq are already top-level frame fields).
        j.remove(kChannelKey).remove(kSeqKey);
        const std::string values_json = j.dump();

        std::lock_guard<std::mutex> lk(mu_);
        Channel& ch = channels_[channel];
        ch.json = values_json;
        ch.seq  = seq;
        ch.images.clear();
        for (auto& [key, img] : in.images()) {
            if (img.empty()) continue;
            ch.images[key] = xi::Image(img.width, img.height, img.channels, img.data());  // own for pull
        }
        ++ch.seen;

        // Subscription gating: only encode + push for channels someone is watching.
        if (subscribed_.count(channel))
            emit_binary(build_frame_(channel, ch));

        return xi::Record().set("channel", channel).set("seen", (int64_t)ch.seen);
    }

    // polaris2 wave-2 (docs/new_gen/08 Wave 2 step 3): the xi.pack@1 pack-in/
    // pack-out door. expose is a SINK — it consumes a sealed pack and its real
    // output is the emit_binary side-effect (exactly as process() above returns a
    // small ack while emit_binary pushes the real payload). So this pack-door
    // process() walks the input pack GENERICALLY (count()+key_at()+tag_at()+typed reads — zero
    // producer knowledge; the r2 constraint made real) and returns a small ack
    // pack {channel, seen}, mirroring the Record path's ack record.
    //
    // The framework-internal reserved keys ($channel/$seq) are lifted to the wire
    // frame's top-level fields; every OTHER entry is dumped by tag: scalars/str
    // pass through, image entries get the JPEG preview treatment (v1) or are
    // inlined as raw bin (v3), and nested msgpack rides verbatim (v3). Which wire
    // version is produced is the opt-in `frame_wire_v3` config (DEFAULT v1 — the
    // wire-breaking default stays out per the plan's governance).
    void process(xi::PackIn& in, xi::PackOut& out) override {
        const std::string channel(in.str(xi::Record::kChannelKey).value_or("default"));
        const uint64_t    seq = (uint64_t)in.i64_or(xi::Record::kSeqKey, 0);

        bool v3;
        { std::lock_guard<std::mutex> lk(mu_); v3 = wire_v3_; }

        // Walk the borrowed input pack WITHOUT our mutex (it is immutable +
        // host-owned); take the lock only to publish the result + read state.
        std::vector<uint8_t> frame = v3 ? build_v3_from_pack_(in, channel, seq)
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
                    .set("image_count", (int)ch.images.size())
                    .set("subscribed", subscribed_.count(name) != 0));
            return xi::Json::object().set("count", (int)channels_.size())
                .set("channels", chans).dump();
        }
        if (c == "get" || c == "get_latest") {  // pull latest: the SAME frame, base64'd
            const std::string channel = p["channel"].as_string("default");
            auto it = channels_.find(channel);
            if (it == channels_.end())
                return xi::Json::object().set("found", false).set("channel", channel).dump();
            // A channel fed through the pack door already holds its encoded XEX1
            // frame bytes; the Record path rebuilds from the stored record/images.
            std::vector<uint8_t> frame = !it->second.frame_bytes.empty()
                ? it->second.frame_bytes
                : build_frame_(channel, it->second);
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
            .set("frame_wire_v3", wire_v3_).dump();
    }
    // Config: opt the pack-in door into the canonical XEX1-v3 wire dump
    // (default off — v1 stays the default wire per the plan's governance). The
    // Record path is unaffected; this flag only picks the pack-door encoder.
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return true;   // tolerant: absent/blank def is a no-op
        std::lock_guard<std::mutex> lk(mu_);
        if (p["frame_wire_v3"].valid()) wire_v3_ = p["frame_wire_v3"].as_bool(wire_v3_);
        return true;
    }

private:
    struct Channel {
        std::string                       json = "{}";
        uint64_t                          seq  = 0;
        long long                         seen = 0;
        std::map<std::string, xi::Image>  images;
        std::vector<uint8_t>              frame_bytes;  // latest encoded frame (pack-door path)
    };

    // --- pack-door encoders (the generic walk; docs/new_gen/08 Wave 2) --------

    // XEX1-v3: the canonical frame dump. The generic pack walk + the shared v3
    // encoder now live in xex1_pack_dump.hpp so expose (wire push) and record_save
    // (disk persist) emit BYTE-IDENTICAL bytes for the same pack (doc 10 gate P3);
    // this is the thin call into that ONE implementation.
    std::vector<uint8_t> build_v3_from_pack_(const xi::PackIn& in,
                                              const std::string& channel,
                                              uint64_t seq) const {
        return xi::xex1::encode_pack_v3(in, channel, seq);
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
            if (key == xi::Record::kChannelKey || key == xi::Record::kSeqKey) continue;
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

    // Build the atomic XEX1 frame: magic + msgpack { v, channel, seq, json, images[] }.
    // Framing (msgpack) lives in xex1_encode.hpp; here we JPEG-compress each image
    // (skipping any that fail) and hand the finished entries to the shared encoder.
    std::vector<uint8_t> build_frame_(const std::string& channel, const Channel& ch) const {
        std::vector<xi::xex1::EncImage> encoded;
        encoded.reserve(ch.images.size());
        for (auto& [key, img] : ch.images) {
            std::vector<uint8_t> jpeg = compress_(img);
            if (!jpeg.empty()) encoded.push_back({key, std::move(jpeg)});
        }
        return xi::xex1::encode_frame(channel, ch.seq, ch.json, encoded);
    }

    // JPEG-encode via the host cache, through the SDK xi::Plugin::compress()
    // wrapper: identical images are encoded once globally, and we don't link a
    // codec ourselves. compress() resolves the frozen xi.preview@1 interface
    // (ABI v10 capability segregation) once and caches it, falling back to the
    // legacy host->compress_image field on a pre-v10 host — same host encoder,
    // same bytes, same -needed/0 return convention.
    std::vector<uint8_t> compress_(const xi::Image& img) const {
        if (img.empty()) return {};
        return compress_px_(img.data(), img.width, img.height, img.channels);
    }
    // Same JPEG-through-host-cache path, from raw pixel bytes (the pack-door
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
    bool                            wire_v3_ = false;   // pack-door: emit XEX1-v3 (opt-in)
};

XI_PLUGIN_IMPL(ExposeSink)
// polaris2 wave-2: publish the xi.pack@1 pack-in/pack-out door so the host
// learns expose consumes packs (the generic sink — it walks any sealed pack
// without producer knowledge). The Record process() path is untouched.
XI_PLUGIN_PACK_DOOR(ExposeSink)
