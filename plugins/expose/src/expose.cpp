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

// ---- minimal msgpack encoder (only the types this fixed frame shape needs) ---
using Buf = std::vector<uint8_t>;
void mp_u16(Buf& f, uint16_t v) { f.push_back(v >> 8); f.push_back(v & 0xFF); }   // big-endian
void mp_u32(Buf& f, uint32_t v) { f.push_back(v >> 24); f.push_back((v >> 16) & 0xFF);
                                  f.push_back((v >> 8) & 0xFF); f.push_back(v & 0xFF); }
void mp_u64(Buf& f, uint64_t v) { for (int s = 56; s >= 0; s -= 8) f.push_back((v >> s) & 0xFF); }

void mp_map(Buf& f, uint32_t n) {            // fixmap only (n<=15 — our maps are tiny)
    f.push_back(0x80 | (uint8_t)(n & 0x0F));
}
void mp_arr(Buf& f, uint32_t n) {
    if (n <= 15) f.push_back(0x90 | (uint8_t)n);
    else if (n <= 0xFFFF) { f.push_back(0xDC); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDD); mp_u32(f, n); }
}
void mp_uint(Buf& f, uint64_t v) {
    if (v <= 0x7F) f.push_back((uint8_t)v);
    else if (v <= 0xFF) { f.push_back(0xCC); f.push_back((uint8_t)v); }
    else if (v <= 0xFFFF) { f.push_back(0xCD); mp_u16(f, (uint16_t)v); }
    else if (v <= 0xFFFFFFFFull) { f.push_back(0xCE); mp_u32(f, (uint32_t)v); }
    else { f.push_back(0xCF); mp_u64(f, v); }
}
void mp_str(Buf& f, const std::string& s) {
    size_t n = s.size();
    if (n <= 31) f.push_back(0xA0 | (uint8_t)n);
    else if (n <= 0xFF) { f.push_back(0xD9); f.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { f.push_back(0xDA); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xDB); mp_u32(f, (uint32_t)n); }
    f.insert(f.end(), s.begin(), s.end());
}
void mp_bin(Buf& f, const uint8_t* p, size_t n) {
    if (n <= 0xFF) { f.push_back(0xC4); f.push_back((uint8_t)n); }
    else if (n <= 0xFFFF) { f.push_back(0xC5); mp_u16(f, (uint16_t)n); }
    else { f.push_back(0xC6); mp_u32(f, (uint32_t)n); }
    f.insert(f.end(), p, p + n);
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
            std::vector<uint8_t> frame = build_frame_(channel, it->second);
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
            .set("channels", names).dump();
    }
    bool set_def(const std::string&) override { return true; }

private:
    struct Channel {
        std::string                       json = "{}";
        uint64_t                          seq  = 0;
        long long                         seen = 0;
        std::map<std::string, xi::Image>  images;
    };

    // Build the atomic XEX1 frame: magic + msgpack { v, channel, seq, json, images[] }.
    std::vector<uint8_t> build_frame_(const std::string& channel, const Channel& ch) const {
        Buf f;
        f.push_back('X'); f.push_back('E'); f.push_back('X'); f.push_back('1');
        mp_map(f, 5);
        mp_str(f, "v");       mp_uint(f, 1);
        mp_str(f, "channel"); mp_str(f, channel);
        mp_str(f, "seq");     mp_uint(f, ch.seq);
        mp_str(f, "json");    mp_str(f, ch.json);
        mp_str(f, "images");
        // Pre-encode so the array length is exact (skip images that fail to encode).
        std::vector<std::pair<std::string, std::vector<uint8_t>>> encoded;
        encoded.reserve(ch.images.size());
        for (auto& [key, img] : ch.images) {
            std::vector<uint8_t> jpeg = compress_(img);
            if (!jpeg.empty()) encoded.emplace_back(key, std::move(jpeg));
        }
        mp_arr(f, (uint32_t)encoded.size());
        for (auto& [key, jpeg] : encoded) {
            mp_map(f, 2);
            mp_str(f, "key");  mp_str(f, key);
            mp_str(f, "jpeg"); mp_bin(f, jpeg.data(), jpeg.size());
        }
        return f;
    }

    // JPEG-encode via the host cache, through the SDK xi::Plugin::compress()
    // wrapper: identical images are encoded once globally, and we don't link a
    // codec ourselves. compress() resolves the frozen xi.preview@1 interface
    // (ABI v10 capability segregation) once and caches it, falling back to the
    // legacy host->compress_image field on a pre-v10 host — same host encoder,
    // same bytes, same -needed/0 return convention.
    std::vector<uint8_t> compress_(const xi::Image& img) const {
        if (img.empty()) return {};
        std::vector<uint8_t> jpeg(64 * 1024);
        int n = compress(img.data(), img.width, img.height, img.channels, 85,
                         jpeg.data(), (int)jpeg.size());
        if (n < 0) {  // buffer too small — resize to needed and retry
            jpeg.resize((size_t)(-n));
            n = compress(img.data(), img.width, img.height, img.channels, 85,
                         jpeg.data(), (int)jpeg.size());
        }
        if (n <= 0) return {};
        jpeg.resize((size_t)n);
        return jpeg;
    }

    mutable std::mutex              mu_;
    std::map<std::string, Channel>  channels_;
    std::set<std::string>           subscribed_;
};

XI_PLUGIN_IMPL(ExposeSink)
