#pragma once
//
// xi_abi.hpp — C++ wrapper over the stable C plugin ABI.
//
// Plugin authors write a class deriving from xi::Plugin, override
// process(PackIn&, PackOut&) and exchange(), then put XI_PLUGIN_IMPL(MyClass)
// and XI_PLUGIN_PACK_DOOR(MyClass) at the bottom. The macros generate the C
// entry points and publish the xi.pack@1 data-plane door.
//
// Images are handles managed by the host. The wrapper provides a
// HostImage class that acts like xi::Image but backed by a host handle.
// Copying a HostImage = addref (zero-copy). Destroying = release.
//
// Usage:
//
//   class MyPlugin : public xi::Plugin {
//   public:
//       using xi::Plugin::Plugin;  // inherit ctor
//
//       void process(xi::PackIn& in, xi::PackOut& out) override {
//           auto src = in.image("frame");
//           if (!src) { out.fault("no_frame", "frame"); return; }
//           auto gray = pool_image(src->width, src->height, 1);
//           cv::cvtColor(xi::as_cv_mat(xi::Image(...)), xi::as_cv_mat(gray), cv::COLOR_RGB2GRAY);  // <xi/xi_cv.hpp>
//           out.image("gray", gray.width, gray.height, gray.channels, gray.data());
//           out.boolean("done", true);
//       }
//   };
//
//   XI_PLUGIN_IMPL(MyPlugin)
//   XI_PLUGIN_PACK_DOOR(MyPlugin)
//

#include "xi_abi.h"
#include "xi_image.hpp"
#include "xi_image_blob.hpp"     // xi::ImageBlobView + read_image_blob (xi/image consumer)
#include "xi_json_escape.hpp"    // the ONE JSON string-escape primitive (std-only leaf)
#include "xi_pack_contract.hpp"  // reserved $-keys + fault/provenance helpers (U1, doc 15)

#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xi {

// --- HostImage: an Image backed by a host-managed handle ---
//
// HostImage wraps a xi_image_handle. It refcounts via the host API:
// copy → addref, destroy → release. Data access via host->image_data().
//
// HostImage is implicitly convertible to/from xi::Image so existing
// operator code (toGray, threshold, etc.) works unchanged.

class HostImage {
public:
    HostImage() = default;

    // NOTE: no public (host, handle) constructor. It was a refcount trap:
    // `host->image_create()` returns a handle with refcount=1, so
    // `HostImage(host, host->image_create(...))` would leave refcount=2
    // and leak. Use the two named ctors below:
    //   - from_handle(host, h)  — take ownership of an existing handle
    //   - from_image(host, img) — copy an xi::Image into the host pool
    // For the rare case you genuinely want to share (addref) an existing
    // handle, call `share_handle` explicitly.

    ~HostImage() {
        if (host_ && handle_) host_->image_release(handle_);
    }

    HostImage(const HostImage& o) : host_(o.host_), handle_(o.handle_) {
        if (host_ && handle_) host_->image_addref(handle_);
    }

    HostImage& operator=(const HostImage& o) {
        if (this != &o) {
            if (host_ && handle_) host_->image_release(handle_);
            host_ = o.host_;
            handle_ = o.handle_;
            if (host_ && handle_) host_->image_addref(handle_);
        }
        return *this;
    }

    HostImage(HostImage&& o) noexcept : host_(o.host_), handle_(o.handle_) {
        o.handle_ = XI_IMAGE_NULL;
    }

    HostImage& operator=(HostImage&& o) noexcept {
        if (this != &o) {
            if (host_ && handle_) host_->image_release(handle_);
            host_ = o.host_;
            handle_ = o.handle_;
            o.handle_ = XI_IMAGE_NULL;
        }
        return *this;
    }

    // Convert to xi::Image (copies pixel data — use when you need to
    // pass to operators that expect xi::Image)
    operator Image() const {
        if (!host_ || !handle_) return {};
        int w = host_->image_width(handle_);
        int h = host_->image_height(handle_);
        int c = host_->image_channels(handle_);
        const uint8_t* p = const_cast<const xi_host_api*>(host_)->image_data(handle_);
        return Image(w, h, c, p);
    }

    // Create a HostImage from an xi::Image (copies pixel data into host pool)
    static HostImage from_image(const xi_host_api* host, const Image& img) {
        if (!host || img.empty()) return {};
        xi_image_handle h = host->image_create(img.width, img.height, img.channels);
        if (!h) return {};
        uint8_t* dst = host->image_data(h);
        std::memcpy(dst, img.data(), img.size());
        return from_handle(host, h);  // take existing refcount=1, no addref
    }

    // Take ownership of an existing handle WITHOUT addref (handle already refcount=1)
    static HostImage from_handle(const xi_host_api* host, xi_image_handle h) {
        HostImage hi;
        hi.host_ = host;
        hi.handle_ = h;  // no addref — we take the existing refcount
        return hi;
    }

    // Share an existing handle by addref-ing — for when some other
    // component owns the handle and you want an independently-managed view.
    static HostImage share_handle(const xi_host_api* host, xi_image_handle h) {
        if (host && h) host->image_addref(h);
        return from_handle(host, h);
    }

    bool empty() const { return !handle_ || !host_; }
    xi_image_handle handle() const { return handle_; }
    const xi_host_api* host() const { return host_; }

    uint8_t*       data()     { return host_ ? host_->image_data(handle_) : nullptr; }
    const uint8_t* data() const { return host_ ? host_->image_data(handle_) : nullptr; }
    int width()    const { return host_ ? host_->image_width(handle_) : 0; }
    int height()   const { return host_ ? host_->image_height(handle_) : 0; }
    int channels() const { return host_ ? host_->image_channels(handle_) : 0; }

private:
    const xi_host_api* host_ = nullptr;
    xi_image_handle    handle_ = XI_IMAGE_NULL;
};

// --- xi.pack@1 SDK sugar (polaris2 wave-2) --------------------------------
//
// Thin C++ wrappers over the host xi_pack_v1 C accessors (xi_abi.h). A pack
// crosses the ABI as an OPAQUE HANDLE, so — unlike the in-process Pack
// container (xi_pack.hpp) — reads here resolve by key STRING through the
// host, not by direct container access. Same-DLL container speed is a
// same-DLL property; across the door a plugin reads/writes with its schema's
// key CONSTANTS (still no string literals at call sites, still drift-proof),
// which is what PackIn/PackOut give it.

// The reserved pack keys ($fault/$fault_key/$fault_detail/$src/$prov) and the
// shared fault/provenance helpers now live in xi_pack_contract.hpp (U1,
// docs/new_gen/15) — one home for the SDK, the script surface and the host
// funnel. `xi::pack_contract` keeps its historical name and spellings.

// PackOut — build an output/emit pack through the host builder. Owns the
// builder handle until seal()/abandon; move-only (a builder is single-owner).
class PackOut {
public:
    PackOut() = default;
    // fi4 (the xi.pack@4 blob supplement, spec 30) is OPTIONAL: null on a host
    // with no blob plane — the v1 adders (incl. the image() convenience over the
    // @1 blob-adapter slots) work regardless; the blob adders then fail closed
    // (return false, nothing added).
    explicit PackOut(const xi_pack_v1* fi, const xi_pack_v4* fi4 = nullptr)
        : fi_(fi), fi4_(fi4) {
        if (fi_) b_ = fi_->builder_new();
    }
    ~PackOut() { if (fi_ && b_ && !sealed_) fi_->builder_abandon(b_); }
    PackOut(PackOut&& o) noexcept { move_from(std::move(o)); }
    PackOut& operator=(PackOut&& o) noexcept {
        if (this != &o) { reset_(); move_from(std::move(o)); }
        return *this;
    }
    PackOut(const PackOut&) = delete;
    PackOut& operator=(const PackOut&) = delete;

    bool valid() const { return fi_ && b_ && !sealed_; }

    PackOut& i64(const char* k, int64_t v) { if (valid()) { touched_ = true; fi_->builder_add_i64(b_, k, v); } return *this; }
    PackOut& f64(const char* k, double v)  { if (valid()) { touched_ = true; fi_->builder_add_f64(b_, k, v); } return *this; }
    PackOut& str(const char* k, std::string_view v) {
        if (valid()) { touched_ = true; fi_->builder_add_str(b_, k, v.data(), (int32_t)v.size()); }
        return *this;
    }
    // Real bool entry (tag XI_PACK_TAG_BOOL, canonical 0xc2/0xc3). Falls back to
    // the historical i64 0/1 encoding on a host whose xi_pack_v1 predates the
    // additive bool tail (NULL fn pointer) — readers cover both via bool_or.
    PackOut& boolean(const char* k, bool v) {
        if (!valid()) return *this;
        touched_ = true;
        if (fi_->builder_add_bool) fi_->builder_add_bool(b_, k, v ? 1 : 0);
        else fi_->builder_add_i64(b_, k, v ? 1 : 0);
        return *this;
    }
    PackOut& bin(const char* k, const void* d, size_t n) {
        if (valid()) { touched_ = true; fi_->builder_add_bin(b_, k, d, (int32_t)n); }
        return *this;
    }
    // image()/adopt_image() ride the frozen @1 slots, which are now DOOR
    // ADAPTERS over the blob plane (spec 30): the entry is stored as an
    // "xi/image" self-describing blob, read back by PackIn::image(). adopt_image
    // COPIES the raw pixel handle into a headed blob (a raw buffer has no head —
    // zero-copy image hand-off is now blob_mint + DMA + adopt_blob).
    PackOut& image(const char* k, int32_t w, int32_t h, int32_t c, const void* px) {
        if (valid()) { touched_ = true; fi_->builder_add_image(b_, k, w, h, c, px); }
        return *this;
    }
    PackOut& adopt_image(const char* k, int32_t w, int32_t h, int32_t c, xi_image_handle handle) {
        if (valid()) { touched_ = true; fi_->builder_adopt_image(b_, k, w, h, c, handle); }
        return *this;
    }
    // Nested canonical msgpack (doc 07 D3) — arrays/maps produced by xi::mp::Writer.
    PackOut& mp(const char* k, const void* d, size_t n) {
        if (valid()) { touched_ = true; fi_->builder_add_mp(b_, k, d, (int32_t)n); }
        return *this;
    }

    // ---- xi.pack@4 adders (self-describing blobs, spec 30). Unlike the
    // chainable v1 adders these return bool — the blob verbs are refusable
    // (invalid descriptor, pool exhausted, no @4 on this host), and a payload
    // silently missing from the output would poison the consumer's contract.
    // false = nothing was added.
    //
    // Self-describing blob: `desc` is a canonical msgpack map (build it with
    // xi::BlobDesc / xi::make_image_desc, or reuse a PackIn::Blob's desc bytes);
    // `payload` is copied into the minted buffer's 64B-aligned payload region.
    bool blob(const char* k, const void* desc, int32_t desc_len,
              const void* payload, int64_t payload_len) {
        if (!valid() || !fi4_ || !fi4_->builder_add_blob) return false;
        if (fi4_->builder_add_blob(b_, k, desc, desc_len, payload, payload_len) != 1)
            return false;
        touched_ = true;
        return true;
    }
    // Zero-copy adoption of an already-minted self-describing buffer (e.g. one
    // from blob_mint() filled in place, or the handle a PackIn::blob() view
    // carries) — validates the head, the pack addrefs it.
    bool adopt_blob(const char* k, xi_image_handle handle) {
        if (!valid() || !fi4_ || !fi4_->builder_adopt_blob) return false;
        if (fi4_->builder_adopt_blob(b_, k, handle) != 1) return false;
        touched_ = true;
        return true;
    }
    // Mint a self-describing buffer and expose its 64B-aligned payload region for
    // IN-PLACE fill (camera DMA / SIMD write). Returns the pool handle + writes
    // *payload_out; XI_IMAGE_NULL on a bad descriptor / no @4. The caller fills
    // the payload, then adopt_blob(k, h), then releases its own ref (image
    // release) — the adopt addref'd. This is the zero-copy producer path.
    xi_image_handle blob_mint(const void* desc, int32_t desc_len,
                              int64_t payload_len, void** payload_out) {
        if (!fi4_ || !fi4_->blob_mint) { if (payload_out) *payload_out = nullptr; return XI_IMAGE_NULL; }
        return fi4_->blob_mint(desc, desc_len, payload_len, payload_out);
    }
    // Pack-shaped fail-loud: stamp the reason codes and return. The pack is
    // still a valid, sealed pack the caller routes to a verdict.
    PackOut& fault(const char* code, const char* key, std::string_view detail = {}) {
        str(pack_contract::kFault, code ? code : "");
        if (key && *key)      str(pack_contract::kFaultKey, key);
        if (!detail.empty())  str(pack_contract::kFaultDetail, detail);
        return *this;
    }
    // U1 provenance (doc 15): EXPLICIT producer identity / hop chain. The
    // pack-door glue (pack_door_abi) stamps $src/$prov automatically on every
    // NON-EMPTY door output when the plugin hasn't — call these only to
    // OVERRIDE the automatic stamp (e.g. a router forwarding another
    // producer's payload verbatim), or from a SOURCE that wants origin
    // attribution on an emitted pack (emit() never stamps: replay
    // byte-identity + published pack shapes are producer contracts). Use
    // these setters, not raw str("$src", ...): the glue detects only them.
    PackOut& src(std::string_view id) {
        src_stamped_ = true;
        return str(pack_contract::kSrc, id);
    }
    PackOut& prov(std::string_view chain) {
        prov_stamped_ = true;
        return str(pack_contract::kProv, chain);
    }
    bool src_stamped()  const { return src_stamped_; }
    bool prov_stamped() const { return prov_stamped_; }
    // True iff any adder ran — i.e. the plugin actually produced entries. The
    // door glue stamps provenance only then: an untouched PackOut seals EMPTY
    // (the door's absence sentinel, mirroring the Record path's empty `{}`).
    bool touched() const { return touched_; }

    // Seal into a host-owned pack handle (refcount 1). Empties this PackOut;
    // the CALLER now owns the ref (release it, or the host does after emit/door).
    xi_pack_handle seal() {
        if (!valid()) return XI_PACK_NULL;
        xi_pack_handle h = fi_->builder_seal(b_);
        sealed_ = true;
        b_ = XI_PACK_BUILDER_NULL;
        return h;
    }

    const xi_pack_v1* iface() const { return fi_; }
    const xi_pack_v4* iface4() const { return fi4_; }

private:
    void reset_() { if (fi_ && b_ && !sealed_) fi_->builder_abandon(b_); }
    void move_from(PackOut&& o) noexcept {
        fi_ = o.fi_; fi4_ = o.fi4_; b_ = o.b_; sealed_ = o.sealed_;
        src_stamped_ = o.src_stamped_; prov_stamped_ = o.prov_stamped_;
        touched_ = o.touched_;
        o.fi_ = nullptr; o.fi4_ = nullptr;
        o.b_ = XI_PACK_BUILDER_NULL; o.sealed_ = true;
    }
    const xi_pack_v1* fi_  = nullptr;
    const xi_pack_v4* fi4_ = nullptr;   // xi.pack@4 blob supplement; null with no blob plane
    xi_pack_builder   b_  = XI_PACK_BUILDER_NULL;
    bool               sealed_ = false;
    bool               src_stamped_  = false;   // plugin called src() explicitly
    bool               prov_stamped_ = false;   // plugin called prov() explicitly
    bool               touched_      = false;   // any adder ran (see touched())
};

// PackIn — read a borrowed input pack handle through the host accessors. Does
// NOT own the handle (the host does); borrowed spans are valid for the call.
class PackIn {
public:
    // fi4 (the xi.pack@4 blob supplement, spec 30) is OPTIONAL: null on a host
    // with no blob plane — every v1 reader (incl. image() over the @1 blob-
    // adapter slot) works regardless; the blob readers then report absence.
    PackIn(const xi_pack_v1* fi, xi_pack_handle h, const xi_pack_v4* fi4 = nullptr)
        : fi_(fi), h_(h), fi4_(fi4) {}

    bool valid() const { return fi_ && h_ != XI_PACK_NULL; }
    xi_pack_handle handle() const { return h_; }
    int  count() const { return valid() ? fi_->count(h_) : 0; }
    bool has(const char* k) const { return valid() && fi_->tag_of(h_, k) >= 0; }
    int  tag_of(const char* k) const { return valid() ? fi_->tag_of(h_, k) : -1; }

    // Generic index walk — the count()+key_at()+tag_at() primitives a SINK
    // (expose, record_save) enumerates a sealed pack with WITHOUT any producer
    // knowledge (doc 07 §2 self-description). key_at returns a borrowed,
    // NON-nul-terminated view into the pack slab, valid until the host releases
    // the handle. tag_at is an XI_PACK_TAG_* value (-1 if out of range).
    std::optional<std::string_view> key_at(int i) const {
        if (!valid()) return std::nullopt;
        int32_t n = 0;
        const char* k = fi_->key_at(h_, i, &n);
        if (!k) return std::nullopt;
        return std::string_view(k, n > 0 ? (size_t)n : 0);
    }
    int tag_at(int i) const { return valid() ? fi_->tag_at(h_, i) : -1; }

    // Walk every entry in insertion order as (key, XI_PACK_TAG_* tag) — the
    // generic producer-agnostic path. The key is a borrowed slab view.
    template <class Fn>
    void for_each(Fn&& fn) const {
        int n = count();
        for (int i = 0; i < n; ++i) {
            int32_t kl = 0;
            const char* k = valid() ? fi_->key_at(h_, i, &kl) : nullptr;
            if (!k) continue;
            fn(std::string_view(k, kl > 0 ? (size_t)kl : 0), fi_->tag_at(h_, i));
        }
    }

    std::optional<int64_t> i64(const char* k) const {
        int64_t v; if (valid() && fi_->get_i64(h_, k, &v)) return v; return std::nullopt;
    }
    int64_t i64_or(const char* k, int64_t d) const { auto v = i64(k); return v ? *v : d; }
    // True bool entry (tag XI_PACK_TAG_BOOL). nullopt if absent, not a bool, or
    // the host predates the bool tail.
    std::optional<bool> boolean(const char* k) const {
        int32_t v;
        if (valid() && fi_->get_bool && fi_->get_bool(h_, k, &v)) return v != 0;
        return std::nullopt;
    }
    // Tolerant read: a real bool entry first, then the historical i64 0/1
    // encoding (producers from before the bool tag), else the default.
    bool bool_or(const char* k, bool d) const {
        if (auto b = boolean(k)) return *b;
        auto v = i64(k); return v ? (*v != 0) : d;
    }
    std::optional<double> f64(const char* k) const {
        double v; if (valid() && fi_->get_f64(h_, k, &v)) return v; return std::nullopt;
    }
    std::optional<std::string_view> str(const char* k) const {
        const char* p; int32_t n;
        if (valid() && fi_->get_str(h_, k, &p, &n)) return std::string_view(p, (size_t)n);
        return std::nullopt;
    }
    // Zero-copy image view (dims + pool-buffer pixel span). empty pixels ⇒ absent.
    std::optional<xi_pack_image> image(const char* k) const {
        xi_pack_image img{};
        if (valid() && fi_->get_image(h_, k, &img)) return img;
        return std::nullopt;
    }
    // Raw binary entry bytes — the host resolves inline-slab vs pool-buffer
    // storage to one borrowed span (D1 storage duality). Valid for the call.
    std::optional<std::pair<const uint8_t*, int32_t>> bin(const char* k) const {
        const void* p; int32_t n;
        if (valid() && fi_->get_bin(h_, k, &p, &n))
            return std::make_pair(reinterpret_cast<const uint8_t*>(p), n);
        return std::nullopt;
    }
    // Nested canonical msgpack bytes (decode with xi::mp::Reader).
    std::optional<std::pair<const uint8_t*, int32_t>> mp(const char* k) const {
        const void* p; int32_t n;
        if (valid() && fi_->get_mp(h_, k, &p, &n))
            return std::make_pair(reinterpret_cast<const uint8_t*>(p), n);
        return std::nullopt;
    }

    // ---- xi.pack@4 readers (self-describing blobs, spec 30). nullopt on
    // absence, tag mismatch, or a host without the @4 supplement.
    //
    // Self-describing blob view: the descriptor map bytes + the 64B-aligned
    // payload span, both borrowed into the pool buffer (valid for the call).
    // Decode the descriptor with xi::mp::Reader (or the convention helpers
    // xi::Pack::desc_find_str/desc_find_i64) — "t" is the type string. `handle`
    // is the backing buffer, BORROWED — adopt it via PackOut::adopt_blob for a
    // zero-copy chain (or image_addref it to hold it past the pack).
    struct Blob {
        const uint8_t* desc        = nullptr;
        int32_t        desc_len    = 0;
        const uint8_t* payload     = nullptr;
        int64_t        payload_len = 0;
    };
    std::optional<Blob> blob(const char* k) const {
        if (!valid() || !fi4_ || !fi4_->get_blob) return std::nullopt;
        Blob b;
        const void* d = nullptr; const void* p = nullptr;
        if (fi4_->get_blob(h_, k, &d, &b.desc_len, &p, &b.payload_len) != 1)
            return std::nullopt;
        b.desc    = reinterpret_cast<const uint8_t*>(d);
        b.payload = reinterpret_cast<const uint8_t*>(p);
        return b;
    }
    // Convenience consumer accessor for an `xi/image` self-describing blob
    // (spec 30): parse the descriptor's {w,h,c,dt} + payload into an
    // ImageBlobView in ONE fail-loud call (nullopt on absent key, a non-blob
    // entry, "t" != "xi/image", or a malformed/undersized descriptor). Unlike
    // image() (which rides the @1 xi/image adapter and hands back a u8-assuming
    // xi_pack_image) this carries the real dtype and needs no @1 door. Wrap it as
    // a cv::Mat with xi::as_cv_read(ImageBlobView) from <xi/xi_cv.hpp>.
    std::optional<ImageBlobView> image_blob(const char* k) const {
        auto b = blob(k);
        if (!b) return std::nullopt;
        return read_image_blob(
            std::span<const uint8_t>(b->desc, b->desc_len > 0 ? (size_t)b->desc_len : 0),
            std::span<const uint8_t>(b->payload, b->payload_len > 0 ? (size_t)b->payload_len : 0));
    }
    // The whole i-th directory row in ONE call (key + tag + storage) — the walk
    // primitive; insertion order, exactly like key_at/tag_at.
    std::optional<xi_pack_entry> entry_at(int i) const {
        xi_pack_entry e{};
        if (valid() && fi4_ && fi4_->entry_at && fi4_->entry_at(h_, i, &e))
            return e;
        return std::nullopt;
    }

    // Fail-loud helpers (the PackOut::fault convention, doc 15).
    bool is_fault() const { return has(pack_contract::kFault); }
    std::optional<std::string_view> fault_code() const { return str(pack_contract::kFault); }
    std::optional<std::string_view> fault_key() const { return str(pack_contract::kFaultKey); }
    std::optional<std::string_view> fault_detail() const { return str(pack_contract::kFaultDetail); }
    // U1 provenance (doc 15): the immediate producer / the hop chain.
    std::optional<std::string_view> src() const { return str(pack_contract::kSrc); }
    std::optional<std::string_view> prov() const { return str(pack_contract::kProv); }

    const xi_pack_v1* iface() const { return fi_; }
    const xi_pack_v4* iface4() const { return fi4_; }

private:
    const xi_pack_v1* fi_;
    xi_pack_handle    h_;
    const xi_pack_v4* fi4_ = nullptr;   // xi.pack@4 blob supplement; null with no blob plane
};

// --- Plugin base class ---

class Plugin {
public:
    Plugin(const xi_host_api* host, const std::string& name)
        : host_(host), name_(name) {}

    virtual ~Plugin() = default;

    const xi_host_api* host() const { return host_; }
    const std::string& name() const { return name_; }

    // ABI v8: push an opaque binary frame straight to connected WS clients. The
    // host is a dumb byte pipe — the frame FORMAT is this plugin's contract with
    // its UI (it must self-describe: tag/group/key + dims + codec + payload).
    // Safe from a dispatch worker thread. No-op on a pre-v8 host (null emit_binary).
    //
    // ABI v10 capability segregation (Phase 3): resolves the frozen `xi.emit@1`
    // interface via host->get_interface ONCE and caches it; falls back to the
    // legacy host->emit_binary field on a pre-v10 host. Both reach the identical
    // host byte pipe.
    void emit_binary(const void* data, int len) const {
        if (!(data && len > 0)) return;
        if (const xi_emit_v1* ev = emit_iface()) {
            if (ev->emit_binary) { ev->emit_binary(data, (int32_t)len); return; }
        }
        if (host_ && host_->emit_binary)
            host_->emit_binary(data, (int32_t)len);
    }
    void emit_binary(const std::vector<uint8_t>& frame) const {
        emit_binary(frame.data(), (int)frame.size());
    }

    // ZERO-COPY emit of an owned frame buffer (xi.emit@2 / perf/ws-lean). Hands the
    // host the shared buffer's bytes WITHOUT copying: the host holds a share (via
    // the ownership token below) until the send completes — or the frame is dropped
    // / the connection tears down — then releases it here in THIS (the producer's)
    // TU. A raw 5–20 MP preview is 15–60 MB, so this eliminates a full per-frame
    // memcpy the copying emit_binary would pay. Falls back to the copying path on a
    // host without xi.emit@2. Returns false only for an empty/absent buffer.
    //
    // The token is a heap-allocated COPY of the shared_ptr; owned_shared_release_
    // deletes it (dropping one ref). The host borrows buf->data() until then, so
    // the bytes stay valid and immutable across the async send.
    bool emit_binary_owned(std::shared_ptr<const std::vector<uint8_t>> buf) const {
        if (!buf || buf->empty()) return false;
        if (const xi_emit_v2* ev2 = emit2_iface()) {
            if (ev2->emit_binary_owned) {
                auto* owner =
                    new std::shared_ptr<const std::vector<uint8_t>>(std::move(buf));
                xi_bin_span span{ (*owner)->data(), (int64_t)(*owner)->size() };
                ev2->emit_binary_owned(&span, 1, owner, &owned_shared_release_);
                return true;
            }
        }
        emit_binary(buf->data(), (int)buf->size());   // fallback: copy
        return true;
    }

    // On-disk folder for THIS instance: project/instances/<name>/
    // Already created by the host before this plugin was constructed.
    // Use it to persist files beyond the JSON config returned by get_def.
    // Returns empty string if running detached from a project.
    std::string folder_path() const {
        if (!host_ || !host_->instance_folder) return "";
        char buf[1024];
        int32_t n = host_->instance_folder(name_.c_str(), buf, sizeof(buf));
        if (n > 0) return std::string(buf, (size_t)n);
        if (n < 0) {
            std::vector<char> big((size_t)(-(int64_t)n) + 1);
            n = host_->instance_folder(name_.c_str(), big.data(), (int32_t)big.size());
            if (n > 0) return std::string(big.data(), (size_t)n);
        }
        return "";
    }

    // Publish this instance's latest status string (host keeps it last-value
    // and serves it via cmd:status). No-op on an older host. Keep it short and
    // human: status("grabbing"), status("model loaded, 3 ROIs").
    //
    // ABI v10 capability segregation (Phase 3): set_status now lives in the frozen
    // `xi.log@1` interface; resolve-then-cache with a legacy-field fallback.
    void status(const std::string& text) const {
        if (const xi_log_v1* lv = log_iface()) {
            if (lv->set_status) { lv->set_status(name_.c_str(), text.c_str()); return; }
        }
        if (host_ && host_->set_status) host_->set_status(name_.c_str(), text.c_str());
    }

    // JPEG-encode an image through the host's content-addressed cache (the same
    // image encoded by several plugins, or repeatedly, is encoded ONCE globally).
    // Returns bytes written into `out`, or -needed if `out_cap` is too small
    // (resize + retry), 0 on error / no encoder available.
    //
    // ABI v10 capability segregation (core_fix_plan.md §12 Phase 2): resolves the
    // frozen `xi.preview@1` interface via host->get_interface ONCE and caches it;
    // falls back to the legacy host->compress_image field when the host is pre-v10
    // (no get_interface) or doesn't publish xi.preview. New plugins ride the
    // segregated interface; old plugins and pre-v10 hosts keep working via the
    // field — neither path is privileged, both hit the identical host encoder.
    int compress(const void* px, int w, int h, int ch, int quality,
                 void* out, int out_cap) const {
        if (const xi_preview_v1* pv = preview_iface())
            return pv->compress(px, w, h, ch, quality, out, out_cap);
        if (host_ && host_->compress_image)
            return host_->compress_image(px, w, h, ch, quality, out, out_cap);
        return 0;
    }

    // --- Phase 3 capability wrappers: xi.imaging_rw@1 ------------------------
    // Resolves its frozen interface ONCE via host->get_interface (cached), and
    // falls back to the legacy xi_host_api field on a pre-v10 host. Both paths
    // reach the identical host primitive — neither is privileged.

    // --- xi.imaging_rw@1 read/write access discipline (ext review 02 I.4) ----
    // The blessed low-level pixel accessors that encode read-only-input /
    // writable-output at the ABI. image_read const-qualifies ANY handle's bytes;
    // image_write returns a mutable pointer ONLY for a uniquely-owned output
    // handle (refcount == 1) and NULL for a shared input — no copy-on-write. Both
    // resolve xi.imaging_rw@1 once (cached). On a host that predates the interface
    // they fall back to host->image_data (the legacy always-mutable pointer), so
    // old hosts keep working under the by-convention discipline.
    const uint8_t* image_read(xi_image_handle h) const {
        if (const xi_imaging_rw_v1* rw = imaging_rw_iface()) {
            if (rw->image_read) return rw->image_read(h);
        }
        return host_ ? const_cast<const xi_host_api*>(host_)->image_data(h) : nullptr;
    }
    uint8_t* image_write(xi_image_handle h) const {
        if (const xi_imaging_rw_v1* rw = imaging_rw_iface()) {
            if (rw->image_write) return rw->image_write(h);
        }
        // Pre-interface host: no uniqueness gate available — fall back to the
        // legacy mutable pointer (by-convention discipline).
        return host_ ? host_->image_data(h) : nullptr;
    }

    // --- xi.pack@1 data plane (polaris2 wave-2) -----------------------------
    // Resolve the host Pack interface ONCE (cached); null on a host with no
    // pack plane (then the plugin's pack door is inert — new_pack() is invalid).
    const xi_pack_v1* pack_iface() const {
        if (!pack_resolved_) {
            pack_resolved_ = true;
            if (host_ && host_->get_interface)
                pack_iface_ = static_cast<const xi_pack_v1*>(
                    host_->get_interface("xi.pack", 1));
        }
        return pack_iface_;
    }
    // Resolve the xi.pack@4 blob supplement ONCE (cached) — self-describing
    // blob mint/adopt/add/get + ordinal entry_at (spec 30). Null on a host with
    // no blob plane; the v1 surface (incl. image() over the @1 blob-adapter
    // slots) keeps working either way (PackIn/PackOut blob verbs fail closed).
    const xi_pack_v4* pack4_iface() const {
        if (!pack4_resolved_) {
            pack4_resolved_ = true;
            if (host_ && host_->get_interface)
                pack4_iface_ = static_cast<const xi_pack_v4*>(
                    host_->get_interface("xi.pack", 4));
        }
        return pack4_iface_;
    }
    // Start building an output/emit pack (invalid if the host has no pack
    // plane). Carries the @4 blob supplement when the host publishes it, so
    // out.blob()/adopt_blob()/blob_mint() work out of the box.
    PackOut new_pack() const { return PackOut(pack_iface(), pack4_iface()); }

    // Source side: seal + emit a pack to host dispatch. Consumes `out`. The host
    // takes its own ref for the async event, so we drop ours right after. No-op
    // on a host without the pack plane. The C-ABI field it forwards to is
    // `emit_pack` — the pack data plane is the sole emit door.
    void emit(PackOut&& out, xi_trigger_id id = XI_TRIGGER_NULL, int64_t ts = 0) {
        const xi_pack_v1* fi = pack_iface();
        if (!fi) return;
        // U1 provenance (doc 15): emit() deliberately stamps NOTHING. An
        // emitted pack's entry set is the producer's contract — record_replay
        // re-emits disk dumps BYTE-IDENTICAL (the E3 lossless loop), and a
        // gatherer's published pack shape ({left,right,seq}) must not grow
        // surprise entries. A source that wants origin attribution calls
        // out.src(name()) itself; chains materialise at the first door hop.
        xi_pack_handle h = out.seal();
        if (h == XI_PACK_NULL) return;
        fi->emit_pack(name_.c_str(), id, h, ts);
        fi->release(h);
    }

    // Pack-in/pack-out door: override to consume `in` and fill `out`. Publish
    // it to the host with XI_PLUGIN_PACK_DOOR(YourClass) after XI_PLUGIN_IMPL.
    // This is THE data-plane door — the xi.pack@1 currency is the sole plugin
    // data path (xi::Record was removed in the v12 ABI cut).
    virtual void process(PackIn& in, PackOut& out) { (void)in; (void)out; }

    // SDK plumbing the XI_PLUGIN_PACK_DOOR trampoline calls: wrap the borrowed
    // input handle, run the virtual, seal the output into a host-owned handle the
    // caller (host) takes ownership of. XI_PACK_NULL if the host has no pack plane.
    xi_pack_handle pack_door_abi(xi_pack_handle in) {
        const xi_pack_v1* fi = pack_iface();
        if (!fi) return XI_PACK_NULL;
        PackIn  view(fi, in, pack4_iface());
        PackOut out(fi, pack4_iface());
        process(view, out);
        // U1 provenance (doc 15): every NON-EMPTY door output — result or
        // plugin-minted $fault pack alike — carries producer identity ($src =
        // this instance) and the hop chain ($prov = the input's chain + this
        // hop), stamped BEFORE seal because sealed packs are immutable. Zero
        // plugin-author effort; an explicit PackOut::src()/prov() call wins
        // (a forwarding router can preserve the original attribution). An
        // UNTOUCHED PackOut seals EMPTY on purpose: the empty pack is the
        // door's absence sentinel (the pack mirror of the Record path's `{}`)
        // — provenance rides data; stamping identity onto nothing would turn
        // absence into presence.
        if (out.valid() && out.touched()) {
            if (!out.src_stamped()) out.src(name_);
            if (!out.prov_stamped())
                out.prov(pack_contract::prov_append(
                    pack_contract::prov_parent(fi, in), name_));
        }
        return out.seal();
    }

    // Override these in your plugin:
    virtual std::string exchange(const std::string& cmd_json) { (void)cmd_json; return "{}"; }

    // Helper for the `else` branch of an exchange() if/else-if chain. Returns
    // a JSON object documenting that the command name was not recognised:
    //
    //     {"error":"unknown_command","command":"<name>"}
    //
    // Caller passes the command name they parsed (often via xi_json). This
    // gives drivers a uniform error shape rather than the silent no-op /
    // empty-object that plugin authors otherwise return on fallthrough.
    // P2-4: Surface unknown commands instead of silently dropping them.
    static std::string exchange_unknown_command(const std::string& cmd_name) {
        std::string out = "{\"error\":\"unknown_command\",\"command\":";
        xi::json_escape_into(out, cmd_name);   // the ONE escape primitive
        out += "}";
        return out;
    }
    virtual std::string get_def() const { return "{}"; }
    virtual bool set_def(const std::string& json) { (void)json; return true; }

    // Frame-perfect config swap (ABI v7). Heavy-resource plugins override these
    // AND opt in with XI_PLUGIN_STAGED(Class) so the host exports them:
    //   prepare() — load the new config's heavy assets into a BACKGROUND staging
    //     slot. The host calls this UNGATED (concurrent with process()), so the
    //     load never stalls the pipeline — therefore prepare MUST touch ONLY the
    //     staging slot, never live state read by process(). `folder` is this
    //     instance's resource folder (load big assets from there).
    //   commit() — atomically swap staging → live (a cheap pointer swap). The host
    //     calls this gated / after draining dispatch, so it's uncontended.
    // Defaults: prepare ≡ set_def (immediate, host-gated), commit ≡ no-op — a
    // simple plugin overrides neither and the host falls back automatically.
    virtual bool prepare(const std::string& def, const std::string& folder) {
        (void)folder; return set_def(def);
    }
    virtual void commit() {}

protected:
    // NOTE: the redundant `create_image` (returned a handle-based HostImage) was
    // removed in a v11 API cleanup — `pool_image` below is the sole author-facing
    // allocator. HostImage's named ctors (from_handle/from_image/share_handle) and
    // xi::Image::create_in_pool remain for the SDK's internal marshalling.

    // Allocate a fresh pool slot and return it as a refcounted xi::Image
    // view. Bytes written via the Image's `data()` (or via a cv::Mat from
    // the opt-in `xi::as_cv_mat()`) land directly in pool memory — no
    // heap-to-pool copy when this Image is returned from process(). This is
    // the standard way for plugins to produce an output image:
    //
    //   auto dst = pool_image(src.width, src.height, 1);
    //   cv::GaussianBlur(xi::as_cv_mat(src), xi::as_cv_mat(dst), {0,0}, 2.0);  // <xi/xi_cv.hpp>
    //   out.image("blurred", dst.width, dst.height, dst.channels, dst.data());
    Image pool_image(int w, int h, int ch) {
        return Image::create_in_pool(host_, w, h, ch);
    }

    // Phase 3: log lives in the frozen xi.log@1 interface; resolve-then-cache
    // with a legacy-field fallback (both reach the host log + operator channel).
    void log_at(int32_t level, const std::string& msg) const {
        if (const xi_log_v1* lv = log_iface()) {
            if (lv->log) { lv->log(level, msg.c_str()); return; }
        }
        if (host_ && host_->log) host_->log(level, msg.c_str());
    }
    void log_debug(const std::string& msg) { log_at(0, msg); }
    void log_info(const std::string& msg)  { log_at(1, msg); }
    void log_warn(const std::string& msg)  { log_at(2, msg); }
    void log_error(const std::string& msg) { log_at(3, msg); }

    const xi_host_api* host_;
    std::string name_;

private:
    // Resolve-once cache for the xi.preview@1 interface (used by compress()).
    // get_interface returns a process-stable, host-owned pointer, so caching it
    // once per instance is safe. `resolved_` distinguishes "not yet looked up"
    // from "looked up, absent" so a pre-v10 host is probed at most once.
    const xi_preview_v1* preview_iface() const {
        if (!preview_resolved_) {
            preview_resolved_ = true;
            if (host_ && host_->get_interface)
                preview_ = static_cast<const xi_preview_v1*>(
                    host_->get_interface("xi.preview", 1));
        }
        return preview_;
    }
    mutable bool                 preview_resolved_ = false;
    mutable const xi_preview_v1* preview_          = nullptr;

    // Phase 3 resolve-once caches for the carved domains — same shape as
    // preview_iface(): probe host->get_interface at most once per instance, then
    // cache the process-stable, host-owned pointer (or nullptr on a pre-v10 host,
    // distinguished from "not yet looked up" by the paired `_resolved_` flag).
    const xi_imaging_v1* imaging_iface() const {
        if (!imaging_resolved_) {
            imaging_resolved_ = true;
            if (host_ && host_->get_interface)
                imaging_ = static_cast<const xi_imaging_v1*>(
                    host_->get_interface("xi.imaging", 1));
        }
        return imaging_;
    }
    const xi_imaging_rw_v1* imaging_rw_iface() const {
        if (!imaging_rw_resolved_) {
            imaging_rw_resolved_ = true;
            if (host_ && host_->get_interface)
                imaging_rw_ = static_cast<const xi_imaging_rw_v1*>(
                    host_->get_interface("xi.imaging_rw", 1));
        }
        return imaging_rw_;
    }
    const xi_emit_v1* emit_iface() const {
        if (!emit_resolved_) {
            emit_resolved_ = true;
            if (host_ && host_->get_interface)
                emit_ = static_cast<const xi_emit_v1*>(
                    host_->get_interface("xi.emit", 1));
        }
        return emit_;
    }
    const xi_emit_v2* emit2_iface() const {
        if (!emit2_resolved_) {
            emit2_resolved_ = true;
            if (host_ && host_->get_interface)
                emit2_ = static_cast<const xi_emit_v2*>(
                    host_->get_interface("xi.emit", 2));
        }
        return emit2_;
    }
    // The xi.emit@2 ownership token: a heap-allocated shared_ptr copy. Deleting it
    // (here, in the producer's TU) drops one ref of the frame buffer. Non-capturing
    // → a plain fn pointer the host can call.
    static void owned_shared_release_(void* p) {
        delete static_cast<std::shared_ptr<const std::vector<uint8_t>>*>(p);
    }
    const xi_log_v1* log_iface() const {
        if (!log_resolved_) {
            log_resolved_ = true;
            if (host_ && host_->get_interface)
                log_ = static_cast<const xi_log_v1*>(
                    host_->get_interface("xi.log", 1));
        }
        return log_;
    }
    mutable bool                 imaging_resolved_ = false;
    mutable const xi_imaging_v1* imaging_          = nullptr;
    mutable bool                    imaging_rw_resolved_ = false;
    mutable const xi_imaging_rw_v1* imaging_rw_          = nullptr;
    mutable bool                 emit_resolved_    = false;
    mutable const xi_emit_v1*    emit_             = nullptr;
    mutable bool                 emit2_resolved_   = false;
    mutable const xi_emit_v2*    emit2_            = nullptr;   // xi.emit@2 (zero-copy owned)
    mutable bool                 log_resolved_     = false;
    mutable const xi_log_v1*     log_              = nullptr;
    mutable bool                 pack_resolved_   = false;
    mutable const xi_pack_v1*   pack_iface_            = nullptr;   // xi.pack@1 (wave-2)
    mutable bool                 pack4_resolved_  = false;
    mutable const xi_pack_v4*   pack4_iface_           = nullptr;   // xi.pack@4 (blob plane, spec 30)
};


} // namespace xi

// --- XI_PLUGIN_IMPL macro ---

#define XI_PLUGIN_IMPL(ClassName)                                              \
                                                                               \
extern "C" XI_EXPORT                                               \
void* xi_plugin_create(const xi_host_api* host, const char* name) {            \
    try { return new ClassName(host, name); }                                   \
    catch (const std::exception& e) {                                           \
        std::fprintf(stderr, "[xinsp2] plugin create() threw: %s\n", e.what()); \
    } catch (...) {                                                             \
        std::fprintf(stderr, "[xinsp2] plugin create() threw (non-std)\n");     \
    }                                                                          \
    return nullptr; /* fail-loud: the host logs a skipped instance either way */ \
}                                                                              \
                                                                               \
extern "C" XI_EXPORT                                               \
void xi_plugin_destroy(void* inst) {                                           \
    /* A destructor throwing across the C ABI is UB — swallow it in-plugin. */  \
    try { delete static_cast<ClassName*>(inst); }                               \
    catch (...) { std::fprintf(stderr, "[xinsp2] plugin destructor threw\n"); } \
}                                                                              \
                                                                               \
/* B2 defense-in-depth: every per-call export catches C++ exceptions HERE, in   */ \
/* the plugin's OWN runtime. A throw that escaped across the extern "C" boundary */ \
/* into the host unwinds correctly only for a source-mode plugin (same CRT as   */ \
/* the host); a PREBUILT plugin built against a different MSVC runtime unwinding */ \
/* across the DLL boundary is UB. Catching in-plugin makes the boundary noexcept */ \
/* in practice: the throw is caught in the same runtime that raised it, and the  */ \
/* host sees a safe sentinel instead of a corrupt unwind. Catch bodies allocate  */ \
/* nothing (a bad_alloc catch must not re-throw). THE CUT (v12): the Record      */ \
/* xi_plugin_process trampoline is gone — the sole data plane is the xi.pack@1    */ \
/* door published by XI_PLUGIN_PACK_DOOR (below).                                 */ \
extern "C" XI_EXPORT                                               \
int xi_plugin_exchange(void* inst, const char* cmd,                            \
                       char* rsp, int rsplen) {                                \
    auto* self = static_cast<ClassName*>(inst);                                \
    try {                                                                      \
        std::string r = self->exchange(cmd);                                   \
        int n = (int)r.size();                                                 \
        if (rsplen < n + 1) return -n;                                         \
        std::memcpy(rsp, r.data(), r.size());                                  \
        rsp[r.size()] = 0;                                                     \
        return n;                                                              \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin exchange() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin exchange() threw (non-std)\n");  \
    }                                                                          \
    return 0; /* safe sentinel: empty response, host won't re-grow the buf */  \
}                                                                              \
                                                                               \
extern "C" XI_EXPORT                                               \
int xi_plugin_get_def(void* inst, char* buf, int buflen) {                     \
    auto* self = static_cast<ClassName*>(inst);                                \
    try {                                                                      \
        std::string d = self->get_def();                                       \
        int n = (int)d.size();                                                 \
        if (buflen < n + 1) return -n;                                         \
        std::memcpy(buf, d.data(), d.size());                                  \
        buf[d.size()] = 0;                                                     \
        return n;                                                              \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin get_def() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin get_def() threw (non-std)\n");   \
    }                                                                          \
    return 0; /* safe sentinel: empty def */                                   \
}                                                                              \
                                                                               \
extern "C" XI_EXPORT                                               \
int xi_plugin_set_def(void* inst, const char* json) {                          \
    try { return static_cast<ClassName*>(inst)->set_def(json) ? 0 : -1; }      \
    catch (const std::exception& e) {                                          \
        std::fprintf(stderr, "[xinsp2] plugin set_def() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin set_def() threw (non-std)\n");   \
    }                                                                          \
    return -1; /* sentinel: config rejected */                                 \
}                                                                              \
                                                                               \
/* ABI version stamp — host loader checks this against its own        */ \
/* XI_ABI_VERSION and refuses plugins requesting a newer ABI than     */ \
/* the host provides. Plugins compiled before this stamp existed are  */ \
/* treated as v1 with a "pre-versioning" warning, not refused.        */ \
extern "C" XI_EXPORT                                               \
int xi_plugin_abi_version(void) {                                              \
    return XI_ABI_VERSION;                                                     \
}
/* THE CUT (v12): the xi_yyjson_abi export is gone. It gated the in-process
   yyjson doc-pointer fast path of the Record process() dispatch — a path that
   no longer exists (xi::Record deleted; the data plane is the xi.pack@1 door,
   which crosses the ABI as an opaque handle and needs no layout stamp). */

// Opt into the ABI v7 frame-perfect config swap. Place AFTER XI_PLUGIN_IMPL.
// ONLY use this if your plugin OVERRIDES prepare()/commit() with a real double-
// slot — the host calls prepare UNGATED (concurrent with process), and the
// CONTRACT is that prepare touches the staging slot ONLY, never live state. A
// plugin that doesn't export these (didn't use this macro) is driven through the
// host's gated set_def fallback instead, which is always safe.
#define XI_PLUGIN_STAGED(ClassName)                                            \
extern "C" XI_EXPORT                                               \
int xi_plugin_prepare(void* inst, const char* def_json, const char* folder) {  \
    try {                                                                      \
        return static_cast<ClassName*>(inst)->prepare(def_json ? def_json : "", \
                                                  folder ? folder : "") ? 0 : -1; \
    } catch (const std::exception& e) {                                        \
        std::fprintf(stderr, "[xinsp2] plugin prepare() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin prepare() threw (non-std)\n");   \
    }                                                                          \
    return -1; /* sentinel: staging failed -> host keeps the live config */    \
}                                                                              \
extern "C" XI_EXPORT                                               \
void xi_plugin_commit(void* inst) {                                            \
    try { static_cast<ClassName*>(inst)->commit(); }                           \
    catch (const std::exception& e) {                                          \
        std::fprintf(stderr, "[xinsp2] plugin commit() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin commit() threw (non-std)\n");    \
    }                                                                          \
}

// Publish the xi.pack@1 pack-in/pack-out door (polaris2 wave-2). Place AFTER
// XI_PLUGIN_IMPL, ONLY if your plugin OVERRIDES process(PackIn&,PackOut&).
// It exports xi_plugin_get_interface — the plugin-side capability door (the
// synthesis §3 "pure door" dry run) — which the host probes to learn the plugin
// speaks packs. THE CUT (v12): this is the SOLE data-plane door — the Record
// process() path was removed, so a data-plane plugin MUST publish this.
// The trampoline catches C++ exceptions in-plugin (same defense-in-depth as the
// XI_PLUGIN_IMPL exports): the boundary is noexcept in practice.
#define XI_PLUGIN_PACK_DOOR(ClassName)                                        \
extern "C" xi_pack_handle xi__frame_proc_##ClassName(void* inst,              \
                                                      xi_pack_handle in) {    \
    auto* self = static_cast<ClassName*>(inst);                                \
    try { return self->pack_door_abi(in); }                                  \
    catch (const std::exception& e) {                                          \
        std::fprintf(stderr, "[xinsp2] plugin pack-door process() threw: %s\n", e.what()); \
    } catch (...) {                                                            \
        std::fprintf(stderr, "[xinsp2] plugin pack-door process() threw (non-std)\n"); \
    }                                                                          \
    return XI_PACK_NULL; /* hard failure sentinel (a CONTRACT failure is a    \
                             normal sealed pack carrying a $fault entry) */    \
}                                                                              \
extern "C" XI_EXPORT                                               \
const void* xi_plugin_get_interface(const char* id, uint32_t version) {        \
    if (id && version == 1u && std::strcmp(id, "xi.pack") == 0) {             \
        static const xi_pack_proc_v1 iface = { &xi__frame_proc_##ClassName }; \
        return &iface;                                                         \
    }                                                                          \
    return nullptr;                                                            \
}
