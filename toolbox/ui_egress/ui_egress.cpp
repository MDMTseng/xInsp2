//
// ui_egress.cpp — xi.ui.egress, the LIVE-UI egress service-middleware lib plugin
// (docs/new_gen/31-ui-egress-and-plugin-ui.md).
//
// A lib plugin has NO DATA PLANE: it never emits and nothing routes to it. On
// create it registers the capability "xi.ui.egress" (get_interface
// "xi.cap.provider"@1); producers resolve "xi.cap"@1 and call it BY NAME through
// the host funnel — one line from inside process(), never through this DLL's
// vtable.
//
// THE ROLE (spec 31 "Live UI data"): a producer PUSHES what only human eyes need
// (a preview image) to a UI channel. push writes a LATEST-WINS retained slot and
// returns immediately — it never blocks the inspection lane. An OWN TIMER THREAD
// at the configured UI rate (default 30fps) then flushes each channel's slot:
// handle/content-keyed LRU dedup -> dispatch by the blob descriptor "t"
// (xi/image u8 -> xi.jpeg.encode cap at q80; >2MP box-downscaled to <=1MP first;
// xi/jpeg pass-through; unknown "t" -> a metadata card) -> hand the encoded frame
// to expose (pure transport: channel subscriptions + WS fan-out, drop-not-queue).
//
// FAIL-OPEN AT EVERY SEAM: no jpeg cap -> raw fallback (mirroring expose's E2
// preview); no subscriber -> drop at zero cost (no dedup, no encode); a codec
// $fault -> raw for that image, others unaffected.
//
// STATE + THREADS: egress is the first STATEFUL, own-threaded cap service. The
// per-channel slots and the encode LRU hold RETAINED input packs (xi.pack@1
// retain — an UNTRACKED consumer ref, a plain ++rc that any thread releases with
// a plain --rc, so the reinit/release_as(0) owner-tag hazard does NOT apply to
// these refs). Slots + LRU are drained on teardown and on reload windows. push
// handlers arrive concurrently from arbitrary producer/dispatch threads; the
// timer thread is the sole flusher. All shared state is mutex-guarded; counters
// are atomics.
//
// DEDUP NOTE (imgcodec precedent): the @4 get_blob door surfaces the descriptor +
// payload spans but NOT the pool handle, so — exactly as xi.imgcodec's memo does
// for the same ABI reason — the dedup identity is a content hash (FNV-1a over the
// descriptor + payload). Sealed buffers are immutable, so content IS identity;
// the same image pushed to two channels/ticks encodes ONCE (the `encodes`
// counter is the proof, read via exchange "stats").
//
#include <xi/xi_abi.hpp>
#include <xi/xi_blob_head.hpp>   // blob head format: kBlobMagic / blob_payload_off / put_u32_le (plugin-safe)
#include <xi/xi_json.hpp>
#include <xi/xi_mp.hpp>     // parse the blob descriptor (t/w/h/c/dt)

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "xex1_encode.hpp"   // xi::xex1::V3Entry / encode_frame_v3 (the shared wire encoder)

namespace {
constexpr const char* kVersions = "1";
constexpr const char* kCap      = "xi.ui.egress";

// FNV-1a 64 over a byte span — the content-dedup key (see header note).
inline uint64_t fnv1a(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// The xi/image convention fields read out of a blob descriptor (plugin-safe;
// mp::Reader only). Flat {t,w,h,c,dt} maps; a nested value means "not xi/image".
struct ImgDesc { std::string t, dt; int64_t w = 0, h = 0, c = 0; bool ok = false; };
inline ImgDesc parse_desc(const uint8_t* desc, size_t n) {
    ImgDesc d;
    xi::mp::Reader r(desc, n);
    xi::mp::Element m;
    if (r.next(m) != xi::mp::Status::Ok || m.kind != xi::mp::Kind::Map) return d;
    for (uint32_t i = 0; i < m.len; ++i) {
        xi::mp::Element k, v;
        if (r.next(k) != xi::mp::Status::Ok || k.kind != xi::mp::Kind::Str) return d;
        std::string_view key((const char*)k.data, k.len);
        if (r.next(v) != xi::mp::Status::Ok) return d;
        const bool is_int = (v.kind == xi::mp::Kind::Int || v.kind == xi::mp::Kind::UInt);
        const int64_t iv  = (v.kind == xi::mp::Kind::Int) ? v.i : (int64_t)v.u;
        if      (key == "t"  && v.kind == xi::mp::Kind::Str) d.t  = std::string((const char*)v.data, v.len);
        else if (key == "dt" && v.kind == xi::mp::Kind::Str) d.dt = std::string((const char*)v.data, v.len);
        else if (key == "w"  && is_int) d.w = iv;
        else if (key == "h"  && is_int) d.h = iv;
        else if (key == "c"  && is_int) d.c = iv;
        else if (v.kind == xi::mp::Kind::Array || v.kind == xi::mp::Kind::Map) return d;  // non-flat
    }
    d.ok = true;
    return d;
}
}  // namespace

class UiEgress : public xi::Plugin {
public:
    UiEgress(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        pk_  = pack_iface();     // xi.pack@1 (cached by the base)
        if (host && host->get_interface) {
            pk4_ = static_cast<const xi_pack_v4*>(host->get_interface("xi.pack", 4));
            cap_ = static_cast<const xi_cap_v1*>(host->get_interface("xi.cap", 1));
            provider_ = static_cast<const xi_cap_provider_v1*>(
                host->get_interface("xi.cap.provider", 1));
        }
        if (!pk_ || !pk4_ || !provider_) {
            status("ui_egress: host lacks xi.pack@1/@4 or xi.cap.provider@1 — no capability registered");
            return;
        }
        registered_ = (provider_->register_capability(kCap, &h_push, this) == XI_CAP_REG_OK);
        if (!registered_) { status("ui_egress: xi.ui.egress registration failed (name taken?)"); return; }
        run_.store(true, std::memory_order_release);
        timer_ = std::thread([this] { timer_loop_(); });
        status("ui_egress: providing xi.ui.egress");
    }

    ~UiEgress() override {
        // Stop the flusher first, then drain retained state, then unregister.
        run_.store(false, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(wake_mu_); wake_.notify_all(); }
        if (timer_.joinable()) timer_.join();
        drain_all_();
        if (provider_ && registered_) provider_->unregister_capability(kCap, this);
    }

    // -- control surface ------------------------------------------------------
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string command = p["command"].as_string();
        if (command == "stats") {
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                "{\"pushes\":%lld,\"flushes\":%lld,\"encodes\":%lld,\"dedup_hits\":%lld,"
                "\"dedup_collisions\":%lld,\"dropped_no_sub\":%lld,\"raw_fallbacks\":%lld,"
                "\"raw_passthrough\":%lld,\"flush_errors\":%lld,"
                "\"lru_entries\":%zu,\"registered\":%s}",
                (long long)pushes_.load(), (long long)flushes_.load(),
                (long long)encodes_.load(), (long long)dedup_hits_.load(),
                (long long)dedup_collisions_.load(),
                (long long)dropped_no_sub_.load(), (long long)raw_fallbacks_.load(),
                (long long)raw_passthrough_.load(), (long long)flush_errors_.load(),
                lru_size_(), registered_ ? "true" : "false");
            return buf;
        }
        if (command == "clear") { drain_all_(); return "{\"ok\":true}"; }
        return exchange_unknown_command(command);
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(cfg_mu_);
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "{\"fps\":%d,\"quality\":%d,\"downscale_mp\":%d,\"lru_max\":%d,\"encode\":%s}",
            cfg_.fps, cfg_.quality, cfg_.downscale_mp, cfg_.lru_max,
            cfg_.encode ? "true" : "false");
        return buf;
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::lock_guard<std::mutex> lk(cfg_mu_);
        cfg_.fps          = clampi_(p["fps"].as_int(cfg_.fps), 1, 240);
        cfg_.quality      = clampi_(p["quality"].as_int(cfg_.quality), 1, 100);
        cfg_.downscale_mp = clampi_(p["downscale_mp"].as_int(cfg_.downscale_mp), 1, 64);
        cfg_.lru_max      = clampi_(p["lru_max"].as_int(cfg_.lru_max), 1, 4096);
        cfg_.encode       = p["encode"].as_bool(cfg_.encode);
        return true;
    }

private:
    struct Config {
        int fps = 30; int quality = 80; int downscale_mp = 2; int lru_max = 32;
        // encode=false: RAW PASSTHROUGH — the channel ships the pushed blob
        // verbatim (no jpeg, no downscale, no LRU). The "this channel walks raw"
        // knob (doc 31): pull and push then both see raw, at raw's honest cost.
        // Global for now — becomes per-channel when the override table lands.
        bool encode = true;
    };

    static int clampi_(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

    // ---- the cap handler: latest-wins retained slot, returns immediately -----
    xi_pack_handle push_(xi_pack_handle in) {
        if (in == XI_PACK_NULL) return XI_PACK_NULL;
        int32_t probe = 0;
        if (pk_->get_bool && pk_->get_bool(in, "$probe", &probe) && probe) {
            xi_pack_builder b = pk_->builder_new();
            pk_->builder_add_str(b, "$versions", kVersions, (int32_t)std::strlen(kVersions));
            return pk_->builder_seal(b);
        }
        // $channel names the UI channel; default panel channel is ui/<instance>.
        auto chan = pk_contract_str_(in, xi::pack_contract::kChannel);
        std::string channel = chan.empty() ? ("ui/" + name()) : chan;

        pk_->retain(in);                 // untracked consumer ref — held by the slot
        xi_pack_handle old = XI_PACK_NULL;
        {
            std::lock_guard<std::mutex> lk(slot_mu_);
            xi_pack_handle& slot = slots_[channel];
            old = slot;                  // latest-wins: the previous unflushed frame is superseded
            slot = in;
        }
        if (old != XI_PACK_NULL) pk_->release(old);   // release outside the lock
        pushes_.fetch_add(1, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> lk(wake_mu_); wake_.notify_one(); }

        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_i64(b, "ok", 1);
        return pk_->builder_seal(b);
    }

    // ---- the flusher: own timer thread at the UI rate ------------------------
    void timer_loop_() {
        while (run_.load(std::memory_order_acquire)) {
            int fps;
            { std::lock_guard<std::mutex> lk(cfg_mu_); fps = cfg_.fps; }
            const auto period = std::chrono::milliseconds(1000 / (fps < 1 ? 1 : fps));
            {
                std::unique_lock<std::mutex> lk(wake_mu_);
                wake_.wait_for(lk, period, [this] { return !run_.load(std::memory_order_acquire); });
            }
            if (!run_.load(std::memory_order_acquire)) break;
            // The flusher owns this thread: an exception escaping the functor is
            // std::terminate (whole-backend crash). Everything below (parse_desc,
            // the Encoded byte copies, box_downscale_, encode_frame_v3) allocates
            // and can throw bad_alloc/length_error on a large or crafted preview.
            // Catch here so a single bad frame is dropped, not fatal — the
            // "fail-open at every seam" contract the inbound push path already keeps.
            try { flush_once_(); }
            catch (...) { flush_errors_.fetch_add(1, std::memory_order_relaxed); }
        }
    }

    void flush_once_() {
        // Snapshot + clear every pending slot (latest-wins already applied).
        std::unordered_map<std::string, xi_pack_handle> pending;
        {
            std::lock_guard<std::mutex> lk(slot_mu_);
            // ERASE consumed keys (not just null them): a producer churning
            // distinct $channel names would otherwise grow slots_ without bound
            // (doc 28/31 finding F7). A later push re-materialises the key.
            for (auto it = slots_.begin(); it != slots_.end(); ) {
                if (it->second != XI_PACK_NULL) { pending[it->first] = it->second; it = slots_.erase(it); }
                else ++it;
            }
        }
        for (auto& [channel, in] : pending) {
            flush_channel_(channel, in);
            pk_->release(in);            // release the consumed slot ref
        }
        if (!pending.empty()) flushes_.fetch_add(1, std::memory_order_relaxed);
    }

    void flush_channel_(const std::string& channel, xi_pack_handle in) {
        // (1) Subscription gate — no subscriber -> drop at zero cost.
        if (!channel_subscribed_(channel)) {
            dropped_no_sub_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // (2) Read the blob "image" (descriptor + payload).
        const void* desc = nullptr; int32_t desc_len = 0;
        const void* pay = nullptr;  int64_t pay_len = 0;
        if (!pk4_->get_blob(in, "image", &desc, &desc_len, &pay, &pay_len) || !desc || !pay)
            return;                      // no image blob -> nothing to preview
        ImgDesc d = parse_desc((const uint8_t*)desc, (size_t)desc_len);

        int quality, downscale_mp; bool encode;
        { std::lock_guard<std::mutex> lk(cfg_mu_);
          quality = cfg_.quality; downscale_mp = cfg_.downscale_mp; encode = cfg_.encode; }

        // (2b) encode=false: RAW PASSTHROUGH — ship the pushed blob verbatim
        // (descriptor + payload as-is), bypassing dispatch AND the LRU (raw
        // copies are big and there is no encode to memoize). One straight copy
        // into the wire frame; the client decodes the self-describing buffer.
        if (!encode) {
            raw_passthrough_.fetch_add(1, std::memory_order_relaxed);
            auto enc = std::make_shared<Encoded>();
            enc->is_jpeg = false;
            enc->desc.assign((const uint8_t*)desc, (const uint8_t*)desc + desc_len);
            enc->raw.assign((const uint8_t*)pay, (const uint8_t*)pay + pay_len);
            enc->w = (int32_t)d.w; enc->h = (int32_t)d.h; enc->c = (int32_t)d.c;
            std::vector<uint8_t> frame = build_frame_(channel, enc);
            hand_to_expose_(channel, frame);
            return;
        }

        // (3) Dedup by content identity (descriptor + payload) — reuse the memo.
        uint64_t key = fnv1a((const uint8_t*)pay, (size_t)pay_len,
                             fnv1a((const uint8_t*)desc, (size_t)desc_len));
        // Policy fingerprint: fold in EVERY encode-affecting config param so a
        // live set_def (quality OR downscale_mp) can't serve a preview encoded
        // under the old policy for the same content (doc 28/31 finding F6).
        key ^= (uint64_t)quality * 1099511628211ull;
        key ^= (uint64_t)downscale_mp * 14695981039346656037ull;
        // The identity witness for a hash hit: an INDEPENDENT second hash
        // (different FNV basis) over the same bytes. A single-hash LRU hit
        // serving a colliding OTHER image would show the wrong picture across
        // channels; two independent 64-bit hashes make an accidental joint
        // collision astronomically unlikely — verified on every hit, at one
        // extra linear pass per flush (≪ an encode).
        const uint64_t witness = fnv1a((const uint8_t*)pay, (size_t)pay_len,
                                       fnv1a((const uint8_t*)desc, (size_t)desc_len,
                                             0x9E3779B97F4A7C15ull));
        std::shared_ptr<Encoded> enc = lru_get_(key);
        if (enc && enc->witness == witness && enc->src_pay_len == pay_len) {
            dedup_hits_.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (enc) dedup_collisions_.fetch_add(1, std::memory_order_relaxed);
            enc = dispatch_encode_(d, (const uint8_t*)pay, (size_t)pay_len,
                                   (const uint8_t*)desc, (size_t)desc_len, quality, downscale_mp);
            if (enc) {
                enc->witness     = witness;
                enc->src_pay_len = pay_len;
                lru_put_(key, enc);      // a colliding entry is REPLACED, not served
            }
        }
        if (!enc) return;

        // (4) Build the XEX1 wire frame and hand it to expose (pure transport).
        std::vector<uint8_t> frame = build_frame_(channel, enc);
        hand_to_expose_(channel, frame);
    }

    // The encoded UI representation of one preview image.
    struct Encoded {
        int32_t w = 0, h = 0, c = 0;
        int32_t q = 0;
        bool                 is_jpeg = false;   // true: jpeg preview; false: raw fallback / metadata
        std::vector<uint8_t> jpeg;              // jpeg bytes (is_jpeg)
        std::vector<uint8_t> raw;               // raw u8 pixels (raw fallback)
        std::vector<uint8_t> desc;              // descriptor bytes (metadata card / raw)
        std::string          t;                 // descriptor type (dispatch record)
        uint64_t             witness = 0;       // second content hash (dedup-hit identity)
        int64_t              src_pay_len = -1;  // source payload length (identity)
    };

    // Dispatch by descriptor "t" (spec 31). Fail-open at every seam.
    std::shared_ptr<Encoded> dispatch_encode_(const ImgDesc& d,
                                              const uint8_t* pay, size_t pay_len,
                                              const uint8_t* desc, size_t desc_len,
                                              int quality, int downscale_mp) {
        auto out = std::make_shared<Encoded>();
        out->t = d.t;
        out->desc.assign(desc, desc + desc_len);

        if (d.t == "xi/jpeg") {          // pass-through: the payload IS the jpeg
            out->is_jpeg = true;
            out->jpeg.assign(pay, pay + pay_len);
            out->w = (int32_t)d.w; out->h = (int32_t)d.h; out->c = (int32_t)d.c; out->q = quality;
            return out;
        }
        // Overflow-safe u8 gate: bound each dim to INT32 and compute w*h*c in
        // uint64 with an early wh-bound so the product cannot wrap (a wrapped
        // product happening to equal pay_len would slip a truncated width into
        // box_downscale_/encode → OOB). pay_len <= pool cap (INT32_MAX), so wh
        // exceeding INT32_MAX can never match and is rejected before *c.
        const bool u8_dims_ok =
            d.t == "xi/image" && d.dt == "u8" &&
            d.w > 0 && d.h > 0 && d.c > 0 &&
            d.w <= INT32_MAX && d.h <= INT32_MAX && d.c <= INT32_MAX &&
            (uint64_t)d.w * (uint64_t)d.h <= (uint64_t)INT32_MAX &&
            (uint64_t)d.w * (uint64_t)d.h * (uint64_t)d.c == (uint64_t)pay_len;
        if (u8_dims_ok) {
            int w = (int)d.w, h = (int)d.h, c = (int)d.c;
            const uint8_t* px = pay;
            std::vector<uint8_t> scaled;
            // >Nmp -> box-downscale to <= Nmp/2-ish target (<=1MP default), codec stays pure.
            const int64_t cap_px = (int64_t)downscale_mp * 1000000 / 2;  // downscale_mp=2 -> ~1MP
            if ((int64_t)w * h > cap_px && cap_px > 0) {
                if (box_downscale_(px, w, h, c, cap_px, scaled, w, h)) px = scaled.data();
            }
            std::vector<uint8_t> jpeg; int q_used = quality;
            if (encode_jpeg_cap_(px, w, h, c, quality, jpeg) && !jpeg.empty()) {
                out->is_jpeg = true; out->jpeg = std::move(jpeg);
                out->w = w; out->h = h; out->c = c; out->q = q_used;
                return out;
            }
            // Fail-open: no cap / codec down / $fault -> raw pixels on the wire.
            // The payload here is the (possibly downscaled) px/w/h/c — so the
            // descriptor that rides with it MUST describe the EMITTED dims, not
            // the original. Riding the untouched original descriptor over a
            // downscaled payload made a w*h*c-vs-payload mismatch that passes
            // blob_head_validate (offsets only) and garbles / over-reads on the
            // client (doc 28/31 finding: downscale + raw fail-open). Rebuild it.
            raw_fallbacks_.fetch_add(1, std::memory_order_relaxed);
            out->is_jpeg = false;
            out->raw.assign(px, px + (size_t)w * h * c);
            out->w = w; out->h = h; out->c = c;
            out->desc = image_desc_(w, h, c, "u8");
            return out;
        }
        // Unknown "t" (or a non-u8 image we don't normalize yet) -> a metadata
        // card: the descriptor alone rides, so the UI shows a labelled card.
        out->is_jpeg = false;
        out->w = (int32_t)d.w; out->h = (int32_t)d.h; out->c = (int32_t)d.c;
        return out;
    }

    // Call xi.jpeg.encode (mirrors expose's E2 encode_one_). Fail-open on every
    // documented signal (negative rc, $fault, empty jpeg).
    bool encode_jpeg_cap_(const uint8_t* px, int w, int h, int c, int quality,
                          std::vector<uint8_t>& jpeg_out) {
        if (!cap_) return false;
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_image(b, "image", w, h, c, px);
        pk_->builder_add_i64(b, "quality", (int64_t)quality);
        pk_->builder_add_i64(b, "$v", 1);
        xi_pack_handle req = pk_->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        const int32_t rc = cap_->call("xi.jpeg.encode", req, &rsp);
        pk_->release(req);
        if (rc < 0) { if (rsp != XI_PACK_NULL) pk_->release(rsp); return false; }
        if (rsp == XI_PACK_NULL) return false;
        const char* fp = nullptr; int32_t fn = 0;
        const bool faulted = pk_->get_str(rsp, "$fault", &fp, &fn) && fp;
        const void* jp = nullptr; int32_t jn = 0;
        const bool has = pk_->get_bin(rsp, "jpeg", &jp, &jn) && jp && jn > 0;
        if (faulted || !has) { pk_->release(rsp); return false; }
        jpeg_out.assign((const uint8_t*)jp, (const uint8_t*)jp + jn);
        pk_->release(rsp);
        encodes_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Simple box (area-average) downscale of interleaved u8 pixels to <= cap_px
    // total pixels, preserving aspect. Kept INSIDE egress so the codec stays pure.
    static bool box_downscale_(const uint8_t* src, int sw, int sh, int c, int64_t cap_px,
                               std::vector<uint8_t>& out, int& ow, int& oh) {
        if (sw <= 0 || sh <= 0 || c <= 0) return false;
        double scale = std::sqrt((double)cap_px / ((double)sw * sh));
        if (scale >= 1.0) return false;
        ow = (int)(sw * scale); oh = (int)(sh * scale);
        if (ow < 1) ow = 1; if (oh < 1) oh = 1;
        out.assign((size_t)ow * oh * c, 0);
        for (int y = 0; y < oh; ++y) {
            int sy0 = (int)((int64_t)y * sh / oh), sy1 = (int)((int64_t)(y + 1) * sh / oh);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            for (int x = 0; x < ow; ++x) {
                int sx0 = (int)((int64_t)x * sw / ow), sx1 = (int)((int64_t)(x + 1) * sw / ow);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                for (int ch = 0; ch < c; ++ch) {
                    uint32_t acc = 0, n = 0;
                    for (int sy = sy0; sy < sy1 && sy < sh; ++sy)
                        for (int sx = sx0; sx < sx1 && sx < sw; ++sx) {
                            acc += src[((size_t)sy * sw + sx) * c + ch]; ++n;
                        }
                    out[((size_t)y * ow + x) * c + ch] = (uint8_t)(n ? acc / n : 0);
                }
            }
        }
        return true;
    }

    // Build the XEX1-v3 wire frame carrying the preview (the WS-preview arm for a
    // jpeg, or a raw xi/image blob for the fail-open / metadata cases).
    std::vector<uint8_t> build_frame_(const std::string& channel, const std::shared_ptr<Encoded>& e) {
        std::vector<xi::xex1::V3Entry> entries;
        xi::xex1::V3Entry v;
        v.key = "img";
        v.tag = XI_PACK_TAG_BLOB;
        if (e->is_jpeg) {
            v.preview = true;
            v.pv_w = e->w; v.pv_h = e->h; v.pv_c = e->c; v.pv_q = e->q;
            v.pv_jpeg = e->jpeg.data(); v.pv_len = e->jpeg.size();
        } else {
            // Fail-open / metadata: ride the verbatim self-describing buffer.
            frame_scratch_ = make_blob_buffer_(e->desc, e->raw);
            v.blob = frame_scratch_.data(); v.blob_len = frame_scratch_.size();
        }
        entries.push_back(std::move(v));
        return xi::xex1::encode_frame_v3(channel, ++seq_, entries);
    }

    // Build a canonical xi/image descriptor {t,w,h,c,dt} for the EMITTED dims —
    // the raw fail-open path rebuilds this so the descriptor matches its
    // (possibly downscaled) payload. Same shape as host xi::make_image_desc,
    // built plugin-side with xi::mp::Writer (canonical by construction).
    static std::vector<uint8_t> image_desc_(int w, int h, int c, const char* dt) {
        xi::mp::Writer wr;
        wr.map(5);
        wr.key("t");  wr.str("xi/image");
        wr.key("w");  wr.int_(w);
        wr.key("h");  wr.int_(h);
        wr.key("c");  wr.int_(c);
        wr.key("dt"); wr.str(dt);
        auto b = wr.take();
        return std::vector<uint8_t>(b.data(), b.data() + b.size());
    }

    static std::vector<uint8_t> make_blob_buffer_(const std::vector<uint8_t>& desc,
                                                  const std::vector<uint8_t>& payload) {
        const uint32_t dlen = (uint32_t)desc.size();
        const uint64_t poff = xi::blob_payload_off(dlen);
        std::vector<uint8_t> buf((size_t)poff + payload.size(), 0);
        xi::pack_mp_detail::put_u32_le(buf.data() + 0, xi::kBlobMagic);
        xi::pack_mp_detail::put_u32_le(buf.data() + 4, dlen);
        if (dlen) std::memcpy(buf.data() + 8, desc.data(), dlen);
        if (!payload.empty()) std::memcpy(buf.data() + (size_t)poff, payload.data(), payload.size());
        return buf;
    }

    // ---- expose handoff + subscription gate (spec 31 "hand to expose") -------
    // The ingestion rides the cap plane: expose provides xi.ui.sink@1 (a byte-
    // blind store-and-broadcast). PROBE-THEN-PUSH: the timer only reaches here
    // for a channel with FRESH content (empty slots are skipped in flush_once_),
    // so a cheap subscription probe first keeps "no subscriber -> zero encode"
    // EXACTLY true — we never dedup/encode for an unsubscribed channel. Expose
    // absent (no xi.ui.sink) -> fail-open no-op (probe reads absent -> unsubscribed
    // -> drop; the project's plugin list decides whether live UI exists at all).
    bool channel_subscribed_(const std::string& channel) {
        if (!cap_) return false;
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_str(b, xi::pack_contract::kChannel, channel.c_str(), (int32_t)channel.size());
        xi_pack_handle req = pk_->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        const int32_t rc = cap_->call("xi.ui.sink", req, &rsp);
        pk_->release(req);
        if (rc < 0 || rsp == XI_PACK_NULL) { if (rsp != XI_PACK_NULL) pk_->release(rsp); return false; }
        int64_t sub = 0;
        pk_->get_i64(rsp, "subscribed", &sub);
        pk_->release(rsp);
        return sub != 0;
    }
    void hand_to_expose_(const std::string& channel, const std::vector<uint8_t>& frame) {
        if (!cap_ || frame.empty()) return;
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_str(b, xi::pack_contract::kChannel, channel.c_str(), (int32_t)channel.size());
        pk_->builder_add_bin(b, "frame", frame.data(), (int32_t)frame.size());
        xi_pack_handle req = pk_->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        cap_->call("xi.ui.sink", req, &rsp);
        pk_->release(req);
        if (rsp != XI_PACK_NULL) pk_->release(rsp);
    }

    // ---- retained-state LRU (content-keyed encode memo) ----------------------
    std::shared_ptr<Encoded> lru_get_(uint64_t key) {
        std::lock_guard<std::mutex> lk(lru_mu_);
        auto it = lru_map_.find(key);
        if (it == lru_map_.end()) return nullptr;
        lru_order_.splice(lru_order_.begin(), lru_order_, it->second.second);  // touch (MRU)
        return it->second.first;
    }
    void lru_put_(uint64_t key, const std::shared_ptr<Encoded>& e) {
        std::lock_guard<std::mutex> lk(lru_mu_);
        int cap; { std::lock_guard<std::mutex> lk2(cfg_mu_); cap = cfg_.lru_max; }
        auto it = lru_map_.find(key);
        if (it != lru_map_.end()) { it->second.first = e; lru_order_.splice(lru_order_.begin(), lru_order_, it->second.second); return; }
        lru_order_.push_front(key);
        lru_map_[key] = { e, lru_order_.begin() };
        while ((int)lru_map_.size() > cap) {
            uint64_t victim = lru_order_.back();
            lru_order_.pop_back();
            lru_map_.erase(victim);
        }
    }
    size_t lru_size_() const { std::lock_guard<std::mutex> lk(lru_mu_); return lru_map_.size(); }

    // Drain retained slots + LRU (teardown / reload / clear). Releases every
    // retained input-pack ref (plain --rc — untracked consumer refs).
    void drain_all_() {
        std::vector<xi_pack_handle> release;
        { std::lock_guard<std::mutex> lk(slot_mu_);
          for (auto& [ch, h] : slots_) if (h != XI_PACK_NULL) release.push_back(h);
          slots_.clear(); }
        for (xi_pack_handle h : release) if (pk_) pk_->release(h);
        { std::lock_guard<std::mutex> lk(lru_mu_); lru_map_.clear(); lru_order_.clear(); }
    }

    std::string pk_contract_str_(xi_pack_handle f, const char* key) const {
        const char* p = nullptr; int32_t n = 0;
        if (pk_->get_str && pk_->get_str(f, key, &p, &n) && p) return std::string(p, n > 0 ? (size_t)n : 0);
        return {};
    }

    // The registered handler (funnel-invoked, concurrent). In-plugin catch:
    // XI_PACK_NULL is the hard-failure sentinel.
    static xi_pack_handle h_push(void* self, xi_pack_handle in) {
        try { return static_cast<UiEgress*>(self)->push_(in); }
        catch (...) { return XI_PACK_NULL; }
    }

    const xi_pack_v1*         pk_  = nullptr;
    const xi_pack_v4*         pk4_ = nullptr;
    const xi_cap_v1*          cap_ = nullptr;   // xi.jpeg.encode funnel
    const xi_cap_provider_v1* provider_ = nullptr;
    bool registered_ = false;

    mutable std::mutex cfg_mu_;
    Config cfg_;

    std::mutex slot_mu_;
    std::unordered_map<std::string, xi_pack_handle> slots_;   // channel -> retained latest pack

    mutable std::mutex lru_mu_;
    std::list<uint64_t> lru_order_;   // MRU..LRU
    std::unordered_map<uint64_t, std::pair<std::shared_ptr<Encoded>, std::list<uint64_t>::iterator>> lru_map_;

    std::thread timer_;
    std::atomic<bool> run_{false};
    std::mutex wake_mu_;
    std::condition_variable wake_;
    std::vector<uint8_t> frame_scratch_;   // outlives encode_frame_v3 in build_frame_
    uint64_t seq_ = 0;

    std::atomic<int64_t> pushes_{0}, flushes_{0}, encodes_{0}, dedup_hits_{0},
                         dedup_collisions_{0}, dropped_no_sub_{0}, raw_fallbacks_{0},
                         raw_passthrough_{0}, flush_errors_{0};
};

XI_PLUGIN_IMPL(UiEgress)
