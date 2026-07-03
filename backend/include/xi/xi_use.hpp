#pragma once
//
// xi_use.hpp — script-side proxy to access backend-managed instances.
//
// Scripts call xi::use("cam0") to get a proxy object that routes
// process() and exchange() calls back to the backend's InstanceRegistry
// via C ABI thunks. The backend owns the instance — scripts never
// create or destroy them.
//
// Usage:
//
//   void xi_inspect_entry(int frame) {
//       auto& det = xi::use("det0");
//       // Frames arrive by PUSH — read the current trigger, don't pull:
//       auto img = xi::current_trigger().image("cam0");
//       auto result = det.process(xi::Record().image("gray", img));
//   }
//

#include "xi_abi.h"
#include "xi_clock.hpp"
#include "xi_pack_contract.hpp"  // reserved $-keys + fault/provenance helpers (U1, doc 15)
#include "xi_record.hpp"   // wire codec is yyjson JSON (Record::from_json_bytes / data_json)
#include "xi_image.hpp"
#include "xi_script.hpp"   // XI_SCRIPT_EXPORT (A4 entry export macro)

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ─── A4: explicit-trigger entry (parameterize the entry signature) ──────────
//
// The legacy entry `xi_inspect_entry(int frame)` reads the current trigger from
// an AMBIENT thread_local (via the g_trigger_*_fn_ thunks). That seam is the
// root of Problem A (core_fix_plan-2026-07 §1): a worker thread that calls
// current_trigger() reads a nullptr thread_local and silently gets nothing.
//
// The A4 cure passes the trigger EXPLICITLY. The host fills this C-ABI,
// SELF-CONTAINED view and hands it to the new entry
// `xi_inspect_entry_tv(const xi_trigger_view*, int)`. Everything the script
// needs (image handles + meta doc + host_api to resolve them) is in the struct
// — no thunk, no thread_local — so the xi::Trigger built from it is valid on
// ANY thread and safe to capture by value into xi::async / xi::parallel_for.
//
// Defined at global scope (C-ABI, like xi_host_api / xi_record_image), so both
// the backend (which fills it) and the script (which consumes it) name it the
// same. The `images` array + `meta_doc` are BORROWED for the duration of the
// entry call: the host holds the underlying pool/doc refs; the SDK Trigger
// addref's/retain's its own before the call returns.
struct xi_trigger_view_image {
    const char*     key;     // source name (or "<source>/<image>" for multi-frame)
    xi_image_handle handle;  // borrowed host pool ref (SDK addref's its own)
};

struct xi_trigger_view {
    int32_t                       is_active;      // 0 ⇒ no trigger (plain cmd:run / timer)
    int32_t                       _pad0;
    xi_trigger_id                 id;
    int64_t                       timestamp_us;
    int64_t                       dequeued_at_us;
    const xi_trigger_view_image*  images;         // borrowed, valid for the call only
    int32_t                       image_count;
    int32_t                       _pad1;
    const char*                   leader_source;  // may be null/empty
    void*                         meta_doc;       // yyjson_mut_doc*, borrowed (host holds a ref)
    // polaris2 wave-2 (docs/new_gen/08 Wave 2, step 4): the OPTIONAL v3 Pack this
    // event carries on the dual-carry dispatch path — the SAME xi_pack_handle
    // TriggerEvent::pack holds. XI_PACK_NULL for every Record-era event. Borrowed
    // for the call (the dispatch's ref keeps it alive); the SDK Trigger takes its
    // OWN ref (get_interface("xi.pack",1)->retain) so t.pack() stays valid after.
    xi_pack_handle               pack;          // XI_PACK_NULL ⇒ no pack on this event
    const xi_host_api*            host;           // pool + doc access; null ⇒ inactive
};

// Defined in xi_script_support.hpp (force-included by the compiler)
extern void* g_use_process_fn_;
extern void* g_use_exchange_fn_;
// polaris2 Gate P2 (docs/new_gen/10): xi::use(...).process(ScriptPack) — the
// host callback that drives a plugin's xi.pack@1 pack door. Null on an older
// host (no pack chaining): the ScriptPack overload then returns an empty pack.
extern void* g_use_pack_process_fn_;
extern void* g_use_host_api_;   // xi_host_api* into BACKEND's ImagePool
extern void* g_trigger_info_fn_;
extern void* g_trigger_image_fn_;
extern void* g_trigger_sources_fn_;
extern void* g_trigger_leader_fn_;
extern void* g_trigger_meta_fn_;
// polaris2 gate P2 (expose-from-script): host thunk for xi::use(sink).push(pack).
// Set via the OPTIONAL export xi_script_set_use_push_pack_callback; null on an
// older host ⇒ push() returns false (degrades like every other optional callback).
extern void* g_use_push_pack_fn_;

namespace xi {

// Microseconds since the Unix epoch (system_clock). Same clock the host
// uses to stamp TriggerEvent::timestamp_us / dequeued_at_us, so script-
// side subtraction across the host/script boundary is meaningful.
// Defined here (and not pulled from xi_trigger_bus.hpp) so scripts don't
// have to include host-only headers to compute the latency split.
// Guarded so the host's xi_trigger_bus.hpp can also define it without
// ODR conflict when both headers land in the same TU.
#ifndef XI_NOW_US_DEFINED
#define XI_NOW_US_DEFINED
// Thin compat aliases over the canonical clock (xi_clock.hpp). Both now_us and
// steady_now_us are defined together so the guard can't leave one undefined
// depending on header include order.
inline int64_t now_us()        { return xi::wall_us(); }
inline int64_t steady_now_us() { return xi::mono_us(); }
#endif

// Function pointer types for the callbacks
// γ: `input_doc` carries the caller's borrowed yyjson_mut_doc* for the in-process
// zero-serialize path. The host callback uses it directly when the target plugin
// is doc-compatible, else serialises it to JSON itself — so the caller never
// pays data_json() up front. `input_data`/`input_len` stay in the signature but
// are null from the doc path (legacy/explicit-JSON callers may still set them).
using UseProcessFn  = int (*)(const char* name,
                              const void* input_doc,
                              const uint8_t* input_data, int32_t input_len,
                              const xi_record_image* input_images, int input_image_count,
                              xi_record_out* output);
using UseExchangeFn = int (*)(const char* name, const char* cmd,
                              char* rsp, int rsplen);
using UseGrabFn     = xi_image_handle (*)(const char* name, int timeout_ms);
// polaris2 Gate P2: drive the named instance's xi.pack@1 pack door with a
// sealed input pack. `in` is BORROWED for the call (the script keeps its ref);
// on success (return 0) `*out` is a NEW sealed pack handle the CALLER OWNS
// (host pack registry — release through xi_pack_v1) or XI_PACK_NULL if the
// plugin's door returned nothing (a hard internal failure; a mere CONTRACT
// failure is a normal sealed pack carrying "$fault", see xi_pack_proc_v1).
// Negative returns mirror UseProcessFn: -1 no such instance; -2 the door
// crashed (SEH) or threw; -3 instance quarantined (on_fault=refuse);
// -4 the plugin publishes no xi.pack@1 door (a Record-only plugin).
using UsePackProcessFn = int (*)(const char* name, xi_pack_handle in,
                                 xi_pack_handle* out);
// polaris2 gate P2 (expose-from-script): push a SEALED pack to a named
// instance's xi.pack@1 door, fire-and-forget (the ack pack is dropped host-
// side, mirroring how a staged sink's reply Record is dropped). The handle is
// BORROWED for the call — the host retains its own ref before returning, so
// the script's ScriptPack ref (t.pack()'s keepalive) is the only one the SDK
// side manages. Returns: 0 staged/delivered; -1 no such instance; -2 the door
// crashed; -3 instance quarantined; -4 instance has no xi.pack@1 door.
using UsePushPackFn = int (*)(const char* name, xi_pack_handle pack);

// Trigger callbacks (host-side wires these in via xi_script_set_trigger_callbacks)
struct CurrentTriggerInfo {
    xi_trigger_id id;
    int64_t       timestamp_us;     // emit timestamp (host->emit_trigger ts)
    int32_t       is_active;        // 0 if no trigger is currently being dispatched
    int32_t       _pad = 0;         // align dequeued_at_us to 8 bytes
    int64_t       dequeued_at_us;   // moment dispatcher worker popped this event
                                    // off g_ev_queue (same clock as timestamp_us).
                                    // queue_wait_us = dequeued_at_us - timestamp_us
                                    // inspect_us    = now_us()       - dequeued_at_us
};
using TriggerInfoFn    = void (*)(CurrentTriggerInfo* out);
using TriggerImageFn   = xi_image_handle (*)(const char* source);
using TriggerSourcesFn = int32_t (*)(char* buf, int32_t buflen);
// Returns the policy leader's source name (or empty string when the
// trigger has no leader, e.g. policy=any). Same buf-or-needed convention
// as TriggerSourcesFn: positive return = bytes written, negative return
// = -needed_bytes (caller resizes and retries), 0 = no leader.
using TriggerLeaderFn  = int32_t (*)(char* buf, int32_t buflen);
// ABI v5: returns the event's metadata doc (emit_trigger_record) as a
// yyjson_mut_doc* with one ref RESERVED for the caller to adopt_shared (==
// consume) — exactly the share_out/adopt_shared handshake the process()-input
// doc uses. null when the trigger carries no metadata.
using TriggerMetaFn    = void* (*)();

// ─── EXPERIMENTAL: the wave-2 Pack pilot surface (t.pack()) ─────────────────
//
// A SCRIPT-SIDE, borrowed, read-only view of the v3 Pack (xi_pack.hpp) that a
// pack-mode source emitted on the xi.pack@1 data plane. This is a WAVE-2 PILOT
// surface — minimal, opaque-handle-backed, and SUBJECT TO CHANGE: the final
// script Pack API is a wave-3+ decision (docs/new_gen/08 Wave 2, step 4).
//
// WHY OPAQUE, NOT the TypedPack<Schema> container directly: a script is a
// separate JIT-compiled DLL. The Pack container (xi_pack.hpp) owns an arena +
// pool handles minted host-side, and every inline singleton (PackRegistry) is
// PER-DLL — so the script cannot touch the container's C++ layout. It reads the
// pack through the process-stable xi_pack_v1 vtable it resolves from the host
// (get_interface("xi.pack", 1)): spans in / spans out, keyed by string. The
// 522ns compile-time-offset slot read is a SAME-DLL property (the pure-door
// finding, docs/new_gen/08); across the door a consumer reads by key string.
//
//   XI_INSPECT_ENTRY(t, frame) {
//       auto f = t.pack();                       // borrowed; empty if no pack
//       if (f) {
//           int64_t seq = f.get_i64("seq").value_or(-1);
//           if (auto img = f.get_image("frame")) xi::ok(1);   // verdict on it
//       }
//   }
//
// LIFETIME: the returned ScriptPack holds its OWN ref on the pack handle (the
// Trigger took it via retain() when it was built from the view). Copies are cheap
// (a shared_ptr bump) and keep the pack + its pool buffers alive — safe to hold
// past the dispatch and to capture BY VALUE into xi::async / xi::parallel_for,
// exactly like an xi::Image or a TriggerSnapshot. ABSENT pack (a Record-era
// event, or no pack plane on the host) ⇒ an empty ScriptPack: bool-false, all
// getters std::nullopt — fail-loud in the script's hands, never a crash.

// A borrowed image entry read out of a pack: descriptor + a zero-copy view over
// the pool buffer's pixels. Valid for the life of the owning ScriptPack.
struct ScriptPackImage {
    int32_t width    = 0;
    int32_t height   = 0;
    int32_t channels = 0;
    std::span<const uint8_t> pixels;
};

template <class Schema> class ScriptTypedPack;

class ScriptPack {
public:
    ScriptPack() = default;
    ScriptPack(xi_pack_handle h, const xi_pack_v1* fi,
                std::shared_ptr<const void> keepalive)
        : h_(h), fi_(fi), keep_(std::move(keepalive)) {}

    // A pack is present iff we hold a live handle AND the host's pack vtable.
    bool valid() const { return fi_ && h_ != XI_PACK_NULL; }
    explicit operator bool() const { return valid(); }

    // Number of keyed entries (0 on an empty pack).
    int32_t count() const { return (valid() && fi_->count) ? fi_->count(h_) : 0; }

    // ---- typed borrowed reads (std::nullopt on absent key / type mismatch) ----
    std::optional<int64_t> get_i64(const char* key) const {
        int64_t v = 0;
        return (valid() && fi_->get_i64 && fi_->get_i64(h_, key, &v) == 1)
                   ? std::optional<int64_t>(v) : std::nullopt;
    }
    std::optional<double> get_f64(const char* key) const {
        double v = 0;
        return (valid() && fi_->get_f64 && fi_->get_f64(h_, key, &v) == 1)
                   ? std::optional<double>(v) : std::nullopt;
    }
    std::optional<bool> get_bool(const char* key) const {
        int32_t v = 0;
        return (valid() && fi_->get_bool && fi_->get_bool(h_, key, &v) == 1)
                   ? std::optional<bool>(v != 0) : std::nullopt;
    }
    std::optional<std::string_view> get_str(const char* key) const {
        const char* p = nullptr; int32_t n = 0;
        return (valid() && fi_->get_str && fi_->get_str(h_, key, &p, &n) == 1)
                   ? std::optional<std::string_view>(std::string_view(p ? p : "", n > 0 ? (size_t)n : 0))
                   : std::nullopt;
    }
    std::optional<std::span<const uint8_t>> get_bin(const char* key) const {
        const void* p = nullptr; int32_t n = 0;
        return (valid() && fi_->get_bin && fi_->get_bin(h_, key, &p, &n) == 1)
                   ? std::optional<std::span<const uint8_t>>(
                         std::span<const uint8_t>(static_cast<const uint8_t*>(p), n > 0 ? (size_t)n : 0))
                   : std::nullopt;
    }
    std::optional<ScriptPackImage> get_image(const char* key) const {
        if (!valid() || !fi_->get_image) return std::nullopt;
        xi_pack_image im{};
        if (fi_->get_image(h_, key, &im) != 1) return std::nullopt;
        return ScriptPackImage{ im.width, im.height, im.channels,
            std::span<const uint8_t>(static_cast<const uint8_t*>(im.pixels),
                                     im.length > 0 ? (size_t)im.length : 0) };
    }
    // Opaque nested msgpack pass-through (arrays/maps a producer emitted).
    std::optional<std::span<const uint8_t>> get_mp(const char* key) const {
        const void* p = nullptr; int32_t n = 0;
        return (valid() && fi_->get_mp && fi_->get_mp(h_, key, &p, &n) == 1)
                   ? std::optional<std::span<const uint8_t>>(
                         std::span<const uint8_t>(static_cast<const uint8_t*>(p), n > 0 ? (size_t)n : 0))
                   : std::nullopt;
    }

    // XI_PACK_TAG_* of a key, or -1 if absent (the raw ABI tag — the script
    // consumes the opaque door, so it speaks int tags, not the C++ PackTag enum).
    int32_t tag_of(const char* key) const {
        return (valid() && fi_->tag_of) ? fi_->tag_of(h_, key) : -1;
    }

    // ---- U1 pack-plane error path + provenance (docs/new_gen/15) ------------
    // The pack mirror of Record::is_na()/na_reason() + src(): a fault is a
    // NORMAL sealed pack carrying "$fault" (reason code) — check it before
    // reading results, exactly as the Record path checks is_na(). $src is the
    // immediate producer (the door instance / the funnel hop that minted this
    // pack); $prov is the '/'-joined hop chain, oldest→newest.
    //
    //   auto r = xi::use("det0").process(f);
    //   if (r.is_fault())
    //       xi::ng(1, std::string(*r.fault_reason()).c_str());   // poison, no results
    bool is_fault() const { return pack_contract::is_fault(fi_, valid() ? h_ : XI_PACK_NULL); }
    std::optional<std::string_view> fault_reason() const { return get_str(pack_contract::kFault); }
    std::optional<std::string_view> fault_key()    const { return get_str(pack_contract::kFaultKey); }
    std::optional<std::string_view> fault_detail() const { return get_str(pack_contract::kFaultDetail); }
    std::optional<std::string_view> src()  const { return get_str(pack_contract::kSrc); }
    std::optional<std::string_view> prov() const { return get_str(pack_contract::kProv); }

    // Generic producer-agnostic walk: fn(std::string_view key, int32_t tag) for
    // every entry in insertion order — the same enumeration `expose`/record_save
    // do host-side, exposed to a script that wants to dump an unknown pack.
    template <class Fn>
    void for_each(Fn&& fn) const {
        if (!valid() || !fi_->count || !fi_->key_at || !fi_->tag_at) return;
        int32_t n = fi_->count(h_);
        for (int32_t i = 0; i < n; ++i) {
            int32_t len = 0;
            const char* k = fi_->key_at(h_, i, &len);
            fn(std::string_view(k ? k : "", len > 0 ? (size_t)len : 0), fi_->tag_at(h_, i));
        }
    }

    // The declared-keyset TYPED view: get_i64<Schema::kSeq>() resolves the slot to
    // its key string at COMPILE TIME (Schema::keys[slot]) and reads it by string
    // through the door. Schema is any struct with a constexpr `keys` array (an
    // xi::PackSchema-derived type, or the wave-3 generated _keys, works as-is).
    template <class Schema>
    ScriptTypedPack<Schema> typed() const { return ScriptTypedPack<Schema>(*this); }

    // Escape hatch for callers that want the raw door (e.g. to forward the handle).
    xi_pack_handle    handle() const { return h_; }
    const xi_pack_v1* iface()  const { return fi_; }

private:
    xi_pack_handle             h_    = XI_PACK_NULL;
    const xi_pack_v1*          fi_   = nullptr;
    std::shared_ptr<const void> keep_;   // holds the Trigger's pack ref alive
};

// ScriptTypedPack<Schema> — a schema-keyed convenience over ScriptPack. Reads
// are STILL by key string through the door (the cross-DLL reality); the schema
// only fixes key spelling at compile time and turns a bad slot into a compile
// error. `keys` may hold const char* or std::string_view; both convert to the
// std::string we pass as a NUL-terminated key (a pilot surface — not the hot
// path, so correctness over the micro-cost of the copy).
template <class Schema>
class ScriptTypedPack {
public:
    explicit ScriptTypedPack(ScriptPack f) : f_(std::move(f)) {}
    bool valid() const { return f_.valid(); }
    explicit operator bool() const { return f_.valid(); }
    const ScriptPack& pack() const { return f_; }

    template <int Slot> std::optional<int64_t> get_i64() const {
        static_assert(Slot >= 0 && Slot < (int)Schema::keys.size(), "slot not declared in schema");
        return f_.get_i64(std::string(Schema::keys[Slot]).c_str());
    }
    template <int Slot> std::optional<double> get_f64() const {
        static_assert(Slot >= 0 && Slot < (int)Schema::keys.size(), "slot not declared in schema");
        return f_.get_f64(std::string(Schema::keys[Slot]).c_str());
    }
    template <int Slot> std::optional<bool> get_bool() const {
        static_assert(Slot >= 0 && Slot < (int)Schema::keys.size(), "slot not declared in schema");
        return f_.get_bool(std::string(Schema::keys[Slot]).c_str());
    }
    template <int Slot> std::optional<std::string_view> get_str() const {
        static_assert(Slot >= 0 && Slot < (int)Schema::keys.size(), "slot not declared in schema");
        return f_.get_str(std::string(Schema::keys[Slot]).c_str());
    }
    template <int Slot> std::optional<ScriptPackImage> get_image() const {
        static_assert(Slot >= 0 && Slot < (int)Schema::keys.size(), "slot not declared in schema");
        return f_.get_image(std::string(Schema::keys[Slot]).c_str());
    }

private:
    ScriptPack f_;
};

// xi::Trigger — read-only view of the current inspection event.
//
//   void xi_inspect_entry(int frame) {
//       auto t = xi::current_trigger();
//       if (t.is_active()) {
//           auto img = t.image("cam_left");          // correlated frames
//           auto right = t.image("cam_right");
//           VAR(tid, t.id_string());
//       }
//   }
//
class Trigger {
public:
    // A4: self-contained backing for an EXPLICIT (view-constructed) Trigger.
    // Held by shared_ptr so Trigger copies are cheap and every copy keeps the
    // images (pool refs) + meta (frozen doc ref) alive — safe to capture into a
    // parallel body by value. Null on a default-constructed (ambient) Trigger,
    // in which case the accessors fall back to the thread_local thunks (legacy
    // current_trigger() path).
    struct Data {
        std::unordered_map<std::string, Image> images;
        Record                                 meta;
        CurrentTriggerInfo                     info{};
        std::string                            leader;
        bool                                   active = false;
        // polaris2 wave-2 Pack pilot: an OWNED ref on the event's pack handle
        // (retained from the view). Released here on the LAST Data ref, so every
        // ScriptPack the script kept — even one captured into a worker — stays
        // valid until it (and this Data) drop. XI_PACK_NULL ⇒ no pack carried.
        xi_pack_handle    pack       = XI_PACK_NULL;
        const xi_pack_v1* pack_iface = nullptr;
        ~Data() {
            if (pack != XI_PACK_NULL && pack_iface && pack_iface->release)
                pack_iface->release(pack);
        }
    };

    Trigger() = default;

    // A4: build an explicit, self-contained Trigger from a host-filled view.
    // Addref's each image + retains the meta doc into its OWN refs, so the
    // resulting Trigger (and every copy) stays valid on any thread and past the
    // end of the originating dispatch — no thread_local, no thunk.
    explicit Trigger(const xi_trigger_view* v) {
        if (!v || !v->is_active || !v->host) return;
        auto d = std::make_shared<Data>();
        d->active                = true;
        d->info.id               = v->id;
        d->info.timestamp_us     = v->timestamp_us;
        d->info.dequeued_at_us   = v->dequeued_at_us;
        d->info.is_active        = 1;
        if (v->leader_source) d->leader = v->leader_source;
        for (int i = 0; i < v->image_count; ++i) {
            xi_image_handle h = v->images ? v->images[i].handle : XI_IMAGE_NULL;
            if (h == XI_IMAGE_NULL || !v->images[i].key) continue;
            Image img = Image::adopt_pool_handle(v->host, h);   // addref → our own ref
            if (!img.empty()) d->images.emplace(v->images[i].key, std::move(img));
        }
        if (v->meta_doc && v->host->doc_retain && v->host->doc_release) {
            // Retain our own ref (host keeps its), then adopt_shared FROZEN — the
            // Record consumes this reserved ref and doc_release's it when it dies.
            v->host->doc_retain(v->meta_doc);
            d->meta = Record::adopt_shared((yyjson_mut_doc*)v->meta_doc,
                                           v->host->doc_release, /*frozen=*/true);
        }
        // polaris2 wave-2 Pack pilot: if the event carried a pack, resolve the
        // host's xi.pack@1 vtable and take our OWN ref on the handle (the view's
        // ref is only borrowed for this call). Data's dtor releases it. Mirrors the
        // image/meta addref-our-own discipline so t.pack() survives the dispatch.
        if (v->pack != XI_PACK_NULL && v->host->get_interface) {
            const auto* fi = static_cast<const xi_pack_v1*>(
                v->host->get_interface("xi.pack", 1));
            if (fi && fi->retain && fi->release) {
                fi->retain(v->pack);
                d->pack        = v->pack;
                d->pack_iface = fi;
            }
        }
        data_ = std::move(d);
    }

    bool is_active() const {
        if (data_) return data_->active;
        auto info_fn = reinterpret_cast<TriggerInfoFn>(g_trigger_info_fn_);
        if (!info_fn) return false;
        CurrentTriggerInfo info{};
        info_fn(&info);
        if (!info.is_active) return false;
        info_ = info;
        loaded_ = true;
        return true;
    }

    xi_trigger_id id() const          { if (data_) return data_->info.id;           ensure(); return info_.id; }
    int64_t       timestamp_us() const { if (data_) return data_->info.timestamp_us; ensure(); return info_.timestamp_us; }

    // Microseconds (system_clock, same base as timestamp_us) when the
    // dispatcher worker pulled this event off the internal queue. Useful
    // for separating "time spent waiting in queue" from "time spent in
    // process()":
    //
    //   double queue_wait_us = (double)(t.dequeued_at_us() - t.timestamp_us());
    //   double inspect_us    = (double)(xi::now_us()        - t.dequeued_at_us());
    //
    // 0 if the host hasn't stamped one (e.g. single-shot cmd:run path
    // before this field was introduced, or synthetic timer ticks with
    // no trigger). Always check is_active() first.
    int64_t       dequeued_at_us() const { if (data_) return data_->info.dequeued_at_us; ensure(); return info_.dequeued_at_us; }

    std::string id_string() const {
        if (data_) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                          (unsigned long long)data_->info.id.hi,
                          (unsigned long long)data_->info.id.lo);
            return buf;
        }
        ensure();
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)info_.id.hi,
                      (unsigned long long)info_.id.lo);
        return buf;
    }

    // Returns the named source's image as a zero-copy view over the
    // host pool handle. `fn` returned a fresh ref (refcount=1);
    // adopt_pool_handle addrefs (=2); we release once (=1) — the
    // remaining ref is owned by the returned Image's shared_ptr deleter.
    // Sources with multiple frames-per-trigger use the key
    // "<source>/<image_name>"; for the common single-frame case the
    // key is just the source name.
    Image image(const std::string& source) const {
        if (data_) {
            auto it = data_->images.find(source);
            if (it != data_->images.end()) return it->second;
            if (data_->images.size() == 1) return data_->images.begin()->second;  // sole-image fallback
            return {};
        }
        auto fn = reinterpret_cast<TriggerImageFn>(g_trigger_image_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!fn || !host) return {};
        xi_image_handle h = fn(source.c_str());
        if (h == XI_IMAGE_NULL) return {};
        Image img = Image::adopt_pool_handle(host, h);
        host->image_release(h);
        return img;
    }

    // Source names present in this trigger (\n-separated single allocation).
    //
    // Two-pass against the host: first call with a small stack buffer; if
    // the host returns a negative value (= -needed_bytes), retry once with
    // a heap buffer of the requested size. Avoids both an unconditional
    // heap allocation (common case is <100 bytes) and silent truncation
    // for projects with many or long source names.
    std::vector<std::string> sources() const {
        if (data_) {
            std::vector<std::string> out;
            out.reserve(data_->images.size());
            for (auto& [k, v] : data_->images) out.push_back(k);
            return out;
        }
        auto fn = reinterpret_cast<TriggerSourcesFn>(g_trigger_sources_fn_);
        if (!fn) return {};
        char stack_buf[512];
        int32_t n = fn(stack_buf, sizeof(stack_buf));
        std::vector<std::string> out;
        std::string s;
        if (n > 0) {
            s.assign(stack_buf, (size_t)n);
        } else if (n < 0) {
            int32_t needed = -n;
            std::vector<char> heap((size_t)needed + 1);
            int32_t n2 = fn(heap.data(), (int32_t)heap.size());
            if (n2 <= 0) return out;
            s.assign(heap.data(), (size_t)n2);
        } else {
            return out;
        }
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find('\n', start);
            if (end == std::string::npos) end = s.size();
            if (end > start) out.emplace_back(s.substr(start, end - start));
            start = end + 1;
        }
        return out;
    }

    // The emitting instance's name — the source that emit_record'd this
    // event. Falls back to the first sources() entry when the host has no
    // leader callback (older backends) or returns empty.
    std::string primary_source() const {
        if (data_) {
            if (!data_->leader.empty()) return data_->leader;
            auto srcs = sources();
            return srcs.empty() ? std::string{} : srcs.front();
        }
        auto fn = reinterpret_cast<TriggerLeaderFn>(g_trigger_leader_fn_);
        if (fn) {
            char stack_buf[256];
            int32_t n = fn(stack_buf, sizeof(stack_buf));
            if (n > 0) return std::string(stack_buf, (size_t)n);
            if (n < 0) {
                int32_t needed = -n;
                std::vector<char> heap((size_t)needed + 1);
                int32_t n2 = fn(heap.data(), (int32_t)heap.size());
                if (n2 > 0) return std::string(heap.data(), (size_t)n2);
            }
        }
        // Fallback: first source name (matches policy=any semantics).
        auto srcs = sources();
        return srcs.empty() ? std::string{} : srcs.front();
    }

    // ABI v5: the event's routing/context metadata (whatever the source put in
    // the record it emit_trigger_record'd), as a read-only Record. Zero-copy /
    // zero-serialize — borrows the host-owned metadata doc by pointer (held by
    // the DocRegistry refcount for the life of this dispatch), exactly as a
    // plugin's process(in) borrows in.doc. Read fields off it like any Record:
    //
    //   auto t = xi::current_trigger();
    //   if (t.is_active()) {
    //       auto m = t.meta();
    //       std::string cmd = m["command"].as_string();   // routing key
    //   }
    //
    // Returns an empty Record when the trigger carries no metadata (the bare
    // emit_trigger path) or the host predates this callback. The returned
    // Record is FROZEN (the host still holds its own ref): reads are free,
    // a mutation copy-on-writes into the script's own doc.
    Record meta() const {
        if (data_) return data_->meta;
        auto fn   = reinterpret_cast<TriggerMetaFn>(g_trigger_meta_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!fn || !host || !host->doc_release) return Record();
        void* d = fn();
        if (!d) return Record();
        // adopt_shared CONSUMES the ref trigger_meta_cb reserved for us; the
        // returned Record doc_release's it when it dies, balancing the reserve.
        return Record::adopt_shared((yyjson_mut_doc*)d, host->doc_release, /*frozen=*/true);
    }

    // EXPERIMENTAL (wave-2 Pack pilot, docs/new_gen/08 Wave 2): the v3 Pack this
    // event carried on the dual-carry dispatch path, as a borrowed read-only view
    // (see ScriptPack above). Empty ScriptPack — bool-false, all getters nullopt
    // — when the event carried no pack (a Record-era event, the common case), when
    // the host publishes no xi.pack@1 plane, or on the legacy ambient (non-view)
    // entry: the pilot rides the explicit XI_INSPECT_ENTRY(t, frame) path. Never a
    // crash — the absent-pack contract mirrors t.image()'s empty-Image behaviour.
    ScriptPack pack() const {
        if (data_ && data_->pack != XI_PACK_NULL && data_->pack_iface)
            return ScriptPack(data_->pack, data_->pack_iface, data_);
        return {};
    }

    // True if `name` appears in this trigger's source list. Cheap routing
    // affordance for multi-source scripts that switch on source identity
    // without re-implementing string compare or hashing in the hot path.
    bool has_source(const char* name) const {
        if (!name) return false;
        if (data_) return data_->images.find(name) != data_->images.end();
        for (auto& s : sources()) if (s == name) return true;
        return false;
    }

private:
    void ensure() const {
        if (loaded_) return;
        auto info_fn = reinterpret_cast<TriggerInfoFn>(g_trigger_info_fn_);
        if (!info_fn) return;
        info_fn(&info_);
        loaded_ = true;
    }
    // Explicit (A4) backing — non-null ⇒ view-constructed, thread-safe, no thunk.
    std::shared_ptr<const Data> data_;
    mutable CurrentTriggerInfo info_{};
    mutable bool               loaded_ = false;
};

// Per-call helper. Cheap to construct — internal info is fetched lazily.
inline Trigger current_trigger() { return Trigger{}; }

// xi::TriggerSnapshot — a by-value, thread-safe copy of the current trigger's
// images + meta, captured ON THE INSPECT THREAD for safe use inside parallel
// regions (xi::async / xi::parallel_for / raw #pragma omp).
//
// current_trigger() reads thread_local host state through the trigger thunks and
// is ONLY valid on the inspect thread (Invariant 6.1). A worker thread that calls
// it gets a loud, named error (A1) — or, on an older host, a silent empty result.
// The blessed fix is to snapshot on the inspect thread and capture the snapshot
// BY VALUE into the parallel body:
//
//   auto snap = xi::trigger_snapshot();                              // inspect thread
//   xi::parallel_for(n, [snap](int i){ work(snap.image("gray")); }); // any thread, safe
//
// Each image is held as a pool-backed xi::Image whose shared_ptr keeps one pool
// ref alive for the life of the snapshot (and every copy) — so the pixels stay
// valid across the whole region regardless of when the inspect's own trigger refs
// are dropped. Accessors touch NO thread_local; copies are cheap (shared_ptr +
// frozen-Record refcount bumps) and safe to read concurrently from many threads.
class TriggerSnapshot {
public:
    bool is_active() const { return active_; }

    // Named source's image (empty Image if absent). Mirrors the host thunk's
    // single-image fallback: a one-image trigger resolves by ANY key.
    Image image(const std::string& source) const {
        auto it = images_.find(source);
        if (it != images_.end()) return it->second;
        if (images_.size() == 1)  return images_.begin()->second;
        return {};
    }

    std::vector<std::string> sources() const {
        std::vector<std::string> out;
        out.reserve(images_.size());
        for (auto& [k, v] : images_) out.push_back(k);
        return out;
    }
    bool has_source(const char* name) const {
        return name && images_.find(name) != images_.end();
    }

    // The trigger's metadata Record (frozen, see Trigger::meta()). Held by value
    // so it stays valid for the life of the snapshot, off any thread.
    const Record& meta() const { return meta_; }

    xi_trigger_id id() const           { return info_.id; }
    int64_t       timestamp_us() const { return info_.timestamp_us; }
    int64_t       dequeued_at_us() const { return info_.dequeued_at_us; }
    std::string   id_string() const {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                      (unsigned long long)info_.id.hi,
                      (unsigned long long)info_.id.lo);
        return buf;
    }

private:
    friend TriggerSnapshot trigger_snapshot();
    std::unordered_map<std::string, Image> images_;
    Record                                 meta_;
    CurrentTriggerInfo                     info_{};
    bool                                   active_ = false;
};

// Capture the current trigger's images + meta into a by-value snapshot. MUST be
// called on the inspect thread (where the trigger is ambient); the result is then
// safe to capture into any worker thread. Returns an inactive snapshot when no
// trigger is in flight (plain cmd:run / timer tick).
inline TriggerSnapshot trigger_snapshot() {
    TriggerSnapshot s;
    Trigger t;
    if (!t.is_active()) return s;            // reads thread_local — inspect thread only
    s.active_                = true;
    s.info_.id              = t.id();
    s.info_.timestamp_us    = t.timestamp_us();
    s.info_.dequeued_at_us  = t.dequeued_at_us();
    s.info_.is_active       = 1;
    for (auto& src : t.sources()) {
        Image img = t.image(src);            // addref'd pool view, held by the snapshot
        if (!img.empty()) s.images_.emplace(src, std::move(img));
    }
    s.meta_ = t.meta();                      // frozen Record — own ref, survives dispatch
    return s;
}

// Proxy object returned by xi::use()
// A miss on xi::use("name") — process/exchange returns -1 when no instance by
// that name is registered — used to be silent (empty Record/Image, no log), so a
// typo'd or not-yet-created instance name looked like "found nothing". Surface it
// once per name as an error log so it's discoverable.
inline void warn_use_miss_(const xi_host_api* host, const char* name) {
    if (!host || !host->log) return;
    static std::mutex mu;
    static std::unordered_map<std::string, bool> warned;
    std::string key = name ? name : "";
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!warned.emplace(key, true).second) return;   // warned this name already
    }
    std::string msg = "xi::use(\"" + key + "\"): no such instance — process/exchange "
                      "returns empty (typo, or instance not created yet?)";
    host->log(3, msg.c_str());
}

// polaris2 Gate P2: same once-per-name discipline for a pack-door miss — the
// instance EXISTS but publishes no xi.pack@1 door (a Record-only plugin), so
// use(name).process(ScriptPack) can only return an empty pack. Without this
// log the silent empty looks identical to a real empty result.
inline void warn_use_no_pack_door_(const xi_host_api* host, const char* name) {
    if (!host || !host->log) return;
    static std::mutex mu;
    static std::unordered_map<std::string, bool> warned;
    std::string key = name ? name : "";
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!warned.emplace(key, true).second) return;   // warned this name already
    }
    std::string msg = "xi::use(\"" + key + "\").process(pack): instance has no "
                      "xi.pack@1 door — returns an empty pack (Record-only plugin; "
                      "use the Record process() overload instead)";
    host->log(3, msg.c_str());
}

class UseProxy {
public:
    explicit UseProxy(const std::string& name) : name_(name) {}

    Record process(const Record& input) {
        // NA propagation: a poison input short-circuits — the plugin never runs,
        // and the NA (with its reason) flows straight through. See
        // docs/internals/typed-io.md.
        if (input.is_na()) return Record::na(input.na_reason()).set_src(name_);

        auto process_fn = reinterpret_cast<UseProcessFn>(g_use_process_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!process_fn || !host) return {};

        // Marshal input Record → C ABI. Pool-backed Images forward
        // their existing handle (just addref); heap-backed Images
        // allocate a new pool slot and memcpy the bytes in. The
        // receiving plugin sees the handle either way.
        // Per-thread scratch, reused across calls to drop two allocations off
        // the per-frame dispatch path. Safe because use() calls never nest on a
        // thread: process_fn is a plugin, and a plugin can't call xi::use()
        // (it's script-side), so it can't re-enter this with the buffers live.
        static thread_local std::vector<xi_record_image> in_imgs;
        static thread_local std::vector<xi_image_handle>  in_handles;
        in_imgs.clear();
        in_handles.clear();
        for (auto& [key, img] : input.images()) {
            if (img.empty()) continue;
            xi_image_handle h = XI_IMAGE_NULL;
            if (img.pool_handle() && img.pool_host() == host) {
                h = img.pool_handle();
                host->image_addref(h);
            } else {
                h = host->image_create(img.width, img.height, img.channels);
                if (h == XI_IMAGE_NULL) continue;
                std::memcpy(host->image_data(h), img.data(), img.size());
            }
            in_imgs.push_back({key.c_str(), h});
            in_handles.push_back(h);
        }
        xi_record_out output;
        xi_record_out_init(&output);

        // γ-4: share our input doc into the host registry (enroll this side) and
        // hand the host the registry-managed pointer, so the plugin can adopt it
        // and cache it across frames zero-copy — no serialize. For a JSON-fallback
        // target the host serialises it there; the plugin never adopts, and our
        // enroll ref is released when this input Record dies.
        const void* in_doc = (host->doc_retain && host->doc_release)
            ? (const void*)input.share_out(host->doc_retain, host->doc_release)
            : (const void*)input.doc();
        int prc = process_fn(name_.c_str(), in_doc,
                   nullptr, 0,
                   in_imgs.data(), (int)in_imgs.size(), &output);
        // Release input handles from the BACKEND pool — plugin's process()
        // copied what it needed. Done regardless of outcome.
        for (auto h : in_handles) host->image_release(h);

        // A failed call's `output` is NOT a trustworthy result: -1 = no such
        // instance; -2 = the plugin's process() crashed (SEH) or threw — in which
        // case it may have written a partial/torn out_doc before faulting. Adopting
        // or parsing it would be a use-after-fault, and treating a crashed call as a
        // valid (empty) result is a silent false-pass downstream. Bail with an empty
        // provenance-tagged Record instead of interpreting output. (Previously only
        // -1 was special-cased and the crash path fell through to adopt_shared.)
        if (prc < 0) {
            if (prc == -1 || prc == -3) {
                // -1 = no such instance; -3 = instance quarantined (on_fault=refuse,
                // item 14). Both return BEFORE touching the shared doc — the ref
                // share_out RESERVED for the adopting side is unconsumed, so release
                // it here or it (and the host-owned doc + pooled chunks) leak on every
                // call. -3 skips the "typo'd instance" warning (it's a live instance
                // that's been pulled from service, not a miss).
                if (prc == -1) warn_use_miss_(host, name_.c_str());
                // Scoped to the share_out path: in_doc is the registry pointer only
                // when doc_retain/doc_release were present (else it's a borrowed
                // input.doc() we must NOT release). The JSON-fallback and adopt paths
                // already balance their ref; -2 (crash) is left alone (a torn call may
                // or may not have adopted — don't risk a double-release). That -2 leak
                // is COUNTED host-side, not here: this proxy runs in the script DLL,
                // which has its OWN per-DLL singletons (see g_use_host_api_ note above),
                // so the leaked ref — a HOST DocRegistry ref reserved by share_out — is
                // tallied where it's abandoned and observable: the host dispatch funnel
                // (service_sinks.cpp use_process_inline_), surfaced in dispatch_stats as
                // crash_leaked_docs_lifetime (Q0f; docs/internals/data-layer.md).
                if (host->doc_retain && host->doc_release && in_doc)
                    host->doc_release(const_cast<void*>(in_doc));
            }
            xi_record_out_free(&output);
            Record empty;
            empty.set_src(name_);
            return empty;
        }

        // γ: adopt the borrowed-doc output by pointer (zero parse) when the
        // plugin returned one; otherwise decode the JSON bytes. The adopted doc
        // is host-pool-backed, so freeing it when `result` dies routes through
        // the host (doc->alc) — safe across the DLL boundary.
        Record result = output.out_doc
            ? Record::adopt_shared((yyjson_mut_doc*)output.out_doc, host->doc_release,
                                   host->doc_refcount && host->doc_refcount((void*)output.out_doc) > 1)
            : ((output.data && output.len > 0)
                   ? Record::from_json_bytes(output.data, (size_t)output.len)
                   : Record());
        // Output handles live in the BACKEND pool. Zero-copy: wrap as
        // a pool-backed view (adopt_pool_handle addrefs internally) and
        // release our process_fn ref. Net refcount: still 1, held by
        // the script-side xi::Image via its shared_ptr deleter.
        for (int i = 0; i < output.image_count; ++i) {
            xi_image_handle h = output.images[i].handle;
            if (!h) continue;
            Image img = Image::adopt_pool_handle(host, h);
            host->image_release(h);
            if (!img.empty()) result.image(output.images[i].key, std::move(img));
        }
        xi_record_out_free(&output);
        result.set_src(name_);   // provenance: this output came from this instance
        return result;
    }

    // polaris2 Gate P2 (docs/new_gen/10): chain a Pack into this instance's
    // xi.pack@1 pack door. The pack-plane sibling of process(Record) — same
    // name, same call shape, pack currency in and out:
    //
    //   XI_INSPECT_ENTRY(t, frame) {
    //       auto f = t.pack();                              // pack-mode source
    //       if (!f) return;
    //       auto r = xi::use("det0").process(f);            // drive the door
    //       int64_t blobs = r.get_i64("blob_count").value_or(-1);
    //       if (auto fault = r.get_str("$fault")) xi::ng(1, "det0 fault");
    //   }
    //
    // INPUT: any live ScriptPack — the trigger's own pack (t.pack(), the
    // chaining case above) or a script-built one. Borrowed for the call; the
    // caller's ref is untouched. An EMPTY input returns an empty pack without
    // entering the plugin (absence propagates, never crashes). A FAULT input
    // (is_fault()) short-circuits HOST-SIDE (use_pack_process_cb, doc 15): the
    // plugin never runs and the result is a NEW fault pack carrying the
    // original reason with this instance appended to the $prov chain — the
    // pack mirror of the Record overload's NA short-circuit above.
    //
    // OUTPUT: a ScriptPack that OWNS the door's result handle — the keepalive
    // releases it on the last copy, so (like t.pack()) it is safe to hold past
    // the dispatch and capture by value into xi::async / xi::parallel_for.
    // EMPTY on: older host (no pack callback wired), no such instance (-1,
    // once-per-name error log), door crashed (-2 — a torn result is never
    // adopted), instance quarantined (-3), or a Record-only plugin with no
    // pack door (-4, once-per-name error log). A CONTRACT failure (e.g.
    // missing input key) is NOT empty — the door returns a normal sealed pack
    // carrying "$fault" (fail-loud, xi_pack_proc_v1 contract), so check that.
    //
    // The LEGACY Record path above is byte-for-byte untouched — this is a
    // pure overload on the pack currency.
    ScriptPack process(const ScriptPack& input) {
        auto fn    = reinterpret_cast<UsePackProcessFn>(g_use_pack_process_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!fn || !host) return {};              // older host: no pack chaining
        if (!input.valid()) return {};            // absence propagates
        xi_pack_handle out = XI_PACK_NULL;
        int rc = fn(name_.c_str(), input.handle(), &out);
        if (rc < 0) {
            if (rc == -1) warn_use_miss_(host, name_.c_str());
            else if (rc == -4) warn_use_no_pack_door_(host, name_.c_str());
            // -2 (crash) / -3 (quarantined) are logged host-side where the
            // fault policy runs; `out` from a torn call is never trusted.
            return {};
        }
        if (out == XI_PACK_NULL) return {};       // door's hard internal failure
        // Wrap the result in an OWNING ScriptPack. Prefer the input's cached
        // vtable (same process-stable host singleton); fall back to resolving
        // the door — it must exist, since the host just minted `out` on it.
        const xi_pack_v1* fi = input.iface();
        if (!fi && host->get_interface)
            fi = static_cast<const xi_pack_v1*>(host->get_interface("xi.pack", 1));
        if (!fi || !fi->release) return {};       // unreachable on a sane host
        std::shared_ptr<const void> keep(
            static_cast<const void*>(fi),         // any non-null tag; deleter is the point
            [fi, out](const void*) { fi->release(out); });
        return ScriptPack(out, fi, std::move(keep));
    }

    std::string exchange(const std::string& cmd) {
        auto exchange_fn = reinterpret_cast<UseExchangeFn>(g_use_exchange_fn_);
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!exchange_fn) return "{}";
        // Per-thread scratch, reused (and kept at its high-water size) instead of
        // a fresh 64KB allocation every call. Same non-nesting safety as
        // process(): exchange_fn is a plugin and can't re-enter xi::use().
        static thread_local std::vector<char> buf;
        if (buf.size() < 64 * 1024) buf.resize(64 * 1024);
        int n = exchange_fn(name_.c_str(), cmd.c_str(), buf.data(), (int)buf.size());
        if (n == -1) { warn_use_miss_(host, name_.c_str()); return "{}"; }  // no such instance
        if (n < 0) { buf.resize((size_t)(-(int64_t)n) + 1024);
                     n = exchange_fn(name_.c_str(), cmd.c_str(), buf.data(), (int)buf.size()); }
        return (n > 0) ? std::string(buf.data(), (size_t)n) : "{}";
    }

    // NOTE: the pull-style `grab(timeout_ms)` was removed in a v11 API cleanup.
    // It contradicted the push-only source model: sources push frames via
    // emit_record into the trigger bus, and scripts read the CURRENT trigger
    // (xi::current_trigger().image("src") / the XI_INSPECT_ENTRY view) rather
    // than pulling a frame from an instance. The SDK-side landing pad
    // (g_use_grab_fn_) is now gone too; the host still passes its use_grab_cb
    // stub into the retained grab_fn ABI slot (xi_script_set_use_callbacks),
    // where it is simply discarded until that slot is formally cut.

    // ─── polaris2 gate P2: expose-from-script (push a sealed Pack to a sink) ──
    //
    // Push a pack the script holds (t.pack(), or any ScriptPack) to this
    // instance's xi.pack@1 pack door WITHOUT an intervening plugin:
    //
    //   XI_INSPECT_ENTRY(t, frame) {
    //       if (auto f = t.pack()) xi::use("expose").push(f);
    //   }
    //
    // ZERO-COPY + AS-IS: the sealed pack handle crosses the seam untouched — no
    // re-encode, no re-key, no host stamping ($seq is NOT injected: a sealed
    // pack is immutable, and the whole design point is that expose's XEX1-v2
    // dump of this pack is byte-identical to a direct host-side dump). Routing
    // ($channel) and ordering ($seq) therefore come from the pack's OWN entries;
    // a pack without them lands on expose's "default" channel with seq 0.
    //
    // FIRE-AND-FORGET, sink-ordered: a declared sink target is staged by the
    // host and flushed after the inspect in frame order (the same staged-emit
    // discipline as use(sink).process(rec)); the ack pack is dropped host-side.
    // A non-sink pack-door target is driven inline. Pack-out chaining (reading
    // the door's reply) is NOT this API — that is the use()->door chaining
    // surface, deliberately separate.
    //
    // LIFETIME: `p` is borrowed for the call. The host retains its own ref on
    // the handle before returning (kept until the staged flush releases it), so
    // the script may drop its ScriptPack immediately after push() returns.
    //
    // Returns true iff the push was accepted (staged, or delivered inline);
    // false on an older host (no callback), an empty pack, a missing instance /
    // missing pack door, a quarantined instance, or a crashed inline door.
    bool push(const ScriptPack& p) {
        auto push_fn = reinterpret_cast<UsePushPackFn>(g_use_push_pack_fn_);
        if (!push_fn || !p.valid()) return false;
        int rc = push_fn(name_.c_str(), p.handle());
        if (rc == -1)   // -4 (live instance, no pack door) is NOT a name miss
            warn_use_miss_(reinterpret_cast<const xi_host_api*>(g_use_host_api_),
                           name_.c_str());
        return rc >= 0;
    }

    const std::string& name() const { return name_; }

private:
    std::string name_;
};

// Thread-safe cache of proxies — one per name, lazily created
inline UseProxy& use(const std::string& name) {
    static std::unordered_map<std::string, UseProxy> proxies;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    auto it = proxies.find(name);
    if (it == proxies.end()) {
        it = proxies.emplace(name, UseProxy(name)).first;
    }
    return it->second;
}

} // namespace xi

// ─── A4 entry macro ─────────────────────────────────────────────────────────
//
// The blessed way for a script to opt into the EXPLICIT-trigger entry. Write:
//
//   #include <xi/xi.hpp>
//   #include <xi/xi_use.hpp>
//
//   XI_INSPECT_ENTRY(t, frame) {
//       if (t.is_active()) {
//           auto snap = t;                                   // cheap copy, self-contained
//           xi::parallel_for(n, [snap](int i){ work(snap.image("cam")); });  // safe on any thread
//       }
//   }
//
// This expands to the exported C entry `xi_inspect_entry_tv(const xi_trigger_view*, int)`
// that the host resolves in preference to the legacy `xi_inspect_entry(int)`.
// The host fills the view and passes it explicitly; the macro builds a
// self-contained xi::Trigger from it and hands it to the user body by const ref.
// There is NO ambient thread_local seam on this path — `t` is valid on any
// thread and safe to capture by value into parallel regions.
//
// Legacy scripts that keep `void xi_inspect_entry(int frame)` are unaffected and
// continue to run through the ambient path (compat: the host falls back to the
// old symbol when the new one is absent).
#define XI_INSPECT_ENTRY(tvar, fvar)                                            \
    static void xi_inspect_entry_impl_(const ::xi::Trigger&, int);             \
    XI_SCRIPT_EXPORT void xi_inspect_entry_tv(const ::xi_trigger_view* xi_tv_, \
                                              int xi_frame_) {                 \
        ::xi::Trigger tvar##_obj_(xi_tv_);                                     \
        xi_inspect_entry_impl_(tvar##_obj_, xi_frame_);                        \
    }                                                                          \
    static void xi_inspect_entry_impl_(const ::xi::Trigger& tvar, int fvar)
