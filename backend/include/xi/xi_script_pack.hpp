#pragma once
//
// xi_script_pack.hpp — SCRIPT-SIDE Pack building (polaris2 gate P2,
// docs/new_gen/10-pack-migration-scope.md).
//
// The read side already exists: t.pack() hands the script a ScriptPack
// (xi_use.hpp), a borrowed view over the host's xi.pack@1 opaque-handle door.
// This header adds the PRODUCE side: xi::ScriptPackBuilder lets a script
// assemble its own pack and seal it into that SAME ScriptPack type, so a
// script-built pack is a first-class value in script land — copyable, holdable
// past the dispatch, capturable by value into xi::async / xi::parallel_for,
// and consumable by anything that already accepts a ScriptPack (downstream
// plugin doors, expose, …).
//
//   #include <xi/xi_script_pack.hpp>
//
//   XI_INSPECT_ENTRY(t, frame) {
//       xi::ScriptPackBuilder b;                  // opens a host-side builder
//       b.add_i64("seq", 42);
//       b.add_f64("score", 0.98);
//       b.add_str("label", "part-A");
//       b.add_bin("raw", bytes, n);
//       xi::mp::Writer pts;                       // nested canonical value
//       pts.array(1); pts.map(2);
//       pts.key("x"); pts.int_(3); pts.key("y"); pts.int_(4);
//       b.add_mp("pts", pts);
//       auto pack = b.seal();                     // first-class ScriptPack
//       if (pack) { /* hand it on */ }
//   }
//
// WHY A DOOR, NOT THE C++ PackBuilder DIRECTLY: a script is a separate
// JIT-compiled DLL. The Pack container (xi_pack.hpp) owns an arena + pool
// handles minted host-side, and its inline singletons (PackRegistry,
// pack_pool, the per-thread ArenaPool) are PER-DLL — a script-instantiated
// PackBuilder would build into the WRONG pool/registry and its pack could
// never cross back to the host. So the builder lives host-side behind the
// process-stable xi_pack_v1 vtable (the exact same door plugins build
// through, xi_pack_abi.hpp), reached via the host_api the loader injects at
// DLL load (g_use_host_api_, xi_script_support.hpp). No new thunk, no new
// thread_local: the builder captures the vtable ONCE at construction, so it
// works on any thread the script touches (main entry, xi::async worker,
// xi::parallel_for body) — the same explicit-capture discipline the A4
// trigger view established.
//
// CANONICAL PROFILE, ENFORCED AUTOMATICALLY (doc 07): a script author cannot
// produce non-canonical pack bytes through this surface.
//   * Scalars/str/bin go through the host builder trampolines, which encode
//     via the canonical writer (ints always int64 0xd3, floats 0xcb, str32 /
//     bin32 max-width headers) — the script never supplies encoded bytes.
//   * Nested values (add_mp) are NOT trusted: the bytes are run through
//     xi::mp::canonicalize (ext policy: REJECT ALL) before they reach the
//     builder. A well-behaved xi::mp::Writer value passes byte-identical
//     (canonicalize is the identity on canonical input); narrow/foreign-width
//     forms are re-encoded to the max-width profile; malformed bytes, ext
//     values (a forged pool handle would be a fabricated pointer), non-string
//     map keys and duplicate keys are REFUSED — add_mp returns false and the
//     entry is not added. This is the same gate the host ingress edge applies
//     to foreign bytes (xi_ingress.hpp): the safe path is the only path.
//
// FAIL-CLOSED: on an older host (no xi.pack@1 published) or before the loader
// injected the host_api, the builder is empty — valid() is false, every add_*
// returns false, seal() returns an empty ScriptPack — never a crash. This
// mirrors the absent-pack contract of t.pack().
//
// THREADING: a ScriptPackBuilder is single-writer, exactly like the host
// PackBuilder — build on one thread, then seal. The SEALED ScriptPack is the
// shareable value (copies bump a shared_ptr and the host-side refcount).
//
// LIFETIME: seal() consumes the builder (the host builder id is dead after,
// further add_* return false). The returned ScriptPack's keepalive owns the
// seal's refcount-1: the last ScriptPack copy to drop releases the host pack,
// which frees its arena and pool handles. A builder abandoned without seal()
// (scope exit, early return, exception) tells the host to drop the
// half-built pack — no leak (xi_pack_v1.builder_abandon).
//

#include "xi_mp.hpp"    // xi::mp::Writer / canonicalize (the canonical gate)
#include "xi_use.hpp"   // ScriptPack + xi_pack_v1 (xi_abi.h) + g_use_host_api_

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace xi {

class ScriptPackBuilder {
public:
    // Resolve the host's xi.pack@1 door and open a host-side builder. On a
    // host without the pack plane (or pre-injection) the builder is empty —
    // every operation below fail-closes.
    ScriptPackBuilder() {
        auto* host = reinterpret_cast<const xi_host_api*>(g_use_host_api_);
        if (!host || !host->get_interface) return;
        fi_ = static_cast<const xi_pack_v1*>(host->get_interface("xi.pack", 1));
        if (!fi_ || !fi_->builder_new) { fi_ = nullptr; return; }
        b_ = fi_->builder_new();
    }

    // Move-only (like the host PackBuilder): exactly one owner of the
    // host-side builder id, so scope exit abandons it exactly once.
    ScriptPackBuilder(ScriptPackBuilder&& o) noexcept
        : fi_(o.fi_), b_(o.b_) { o.b_ = XI_PACK_BUILDER_NULL; }
    ScriptPackBuilder& operator=(ScriptPackBuilder&& o) noexcept {
        if (this != &o) {
            abandon();
            fi_ = o.fi_; b_ = o.b_;
            o.b_ = XI_PACK_BUILDER_NULL;
        }
        return *this;
    }
    ScriptPackBuilder(const ScriptPackBuilder&) = delete;
    ScriptPackBuilder& operator=(const ScriptPackBuilder&) = delete;
    ~ScriptPackBuilder() { abandon(); }

    // A builder is usable iff the door resolved and it has not been sealed,
    // moved from, or abandoned.
    bool valid() const { return fi_ && b_ != XI_PACK_BUILDER_NULL; }
    explicit operator bool() const { return valid(); }

    // ---- typed adds (canonical encoding happens host-side) -----------------
    // Every add returns true iff the entry was accepted. Keys are copied by
    // the host (arena-interned), so a temporary key string is fine.

    bool add_i64(const char* key, int64_t v) {
        if (!valid() || !key || !fi_->builder_add_i64) return false;
        fi_->builder_add_i64(b_, key, v);
        return true;
    }
    bool add_f64(const char* key, double v) {
        if (!valid() || !key || !fi_->builder_add_f64) return false;
        fi_->builder_add_f64(b_, key, v);
        return true;
    }
    bool add_str(const char* key, std::string_view v) {
        if (!valid() || !key || !fi_->builder_add_str) return false;
        fi_->builder_add_str(b_, key, v.data(), (int32_t)v.size());
        return true;
    }
    bool add_bin(const char* key, const void* data, size_t n) {
        if (!valid() || !key || !fi_->builder_add_bin) return false;
        if (n && !data) return false;
        fi_->builder_add_bin(b_, key, data, (int32_t)n);
        return true;
    }
    bool add_bin(const char* key, std::span<const uint8_t> v) {
        return add_bin(key, v.data(), v.size());
    }

    // Bool — a REAL bool entry (tag XI_PACK_TAG_BOOL, canonical 0xc2/0xc3)
    // through the additive xi_pack_v1 tail (builder_add_bool). On a host whose
    // pack table predates the bool tail (NULL fn pointer — the tail is
    // appended, so a shorter table reads as NULL here) the value falls back to
    // the historical Mp encoding: one canonical msgpack bool byte, tag Mp,
    // decodable with xi::mp::Reader (Element.kind == Kind::Bool).
    bool add_bool(const char* key, bool v) {
        if (!valid() || !key) return false;
        if (fi_->builder_add_bool) {
            fi_->builder_add_bool(b_, key, v ? 1 : 0);
            return true;
        }
        if (!fi_->builder_add_mp) return false;
        const uint8_t byte = v ? 0xc3 : 0xc2;
        fi_->builder_add_mp(b_, key, &byte, 1);
        return true;
    }

    // Image — pixels are COPIED into a fresh host pool buffer (w*h*c bytes,
    // 8-bit). Zero-copy adoption of an existing pool handle is deliberately
    // not offered here yet: proving pool identity across the script seam is
    // chaining work (the plugin-door sibling), and a wrong-pool adopt would
    // corrupt the refcount ledger. Copy is always correct.
    bool add_image(const char* key, int32_t w, int32_t h, int32_t c,
                   const void* pixels) {
        if (!valid() || !key || !fi_->builder_add_image) return false;
        if (w <= 0 || h <= 0 || c <= 0 || !pixels) return false;
        fi_->builder_add_image(b_, key, w, h, c, pixels);
        return true;
    }
    bool add_image(const char* key, const Image& img) {
        if (img.empty()) return false;
        return add_image(key, img.width, img.height, img.channels, img.read());
    }

    // Nested canonical msgpack value (ONE complete scalar / str / bin / map /
    // array subtree). THE CANONICAL GATE: the bytes are validated AND
    // normalized through xi::mp::canonicalize with the reject-all ext policy
    // before they reach the pack — canonical input (an xi::mp::Writer value)
    // passes byte-identical; non-canonical-width standard msgpack is
    // re-encoded to the max-width profile; malformed / ext-bearing /
    // non-string-keyed / duplicate-keyed bytes are refused (returns false,
    // nothing added). A script cannot place non-canonical bytes in a pack.
    bool add_mp(const char* key, const void* mp, size_t n) {
        if (!valid() || !key || !fi_->builder_add_mp) return false;
        if (!mp || n == 0) return false;
        xi::mp::Writer canon;
        if (xi::mp::canonicalize(static_cast<const uint8_t*>(mp), n, canon)
                != xi::mp::Status::Ok)
            return false;
        fi_->builder_add_mp(b_, key, canon.bytes().data(),
                            (int32_t)canon.bytes().size());
        return true;
    }
    bool add_mp(const char* key, const xi::mp::Writer& w) {
        return add_mp(key, w.bytes().data(), w.bytes().size());
    }
    bool add_mp(const char* key, std::span<const uint8_t> v) {
        return add_mp(key, v.data(), v.size());
    }

    // ---- seal ---------------------------------------------------------------
    // Flip immutable: consume the host builder into a sealed pack and wrap it
    // as a first-class ScriptPack — the SAME type t.pack() yields, so a
    // script-built pack flows anywhere a trigger pack does. The ScriptPack's
    // keepalive owns the seal ref (host refcount 1): copies share it, and the
    // last copy to drop releases the host pack (arena freed, pool handles
    // released). Empty ScriptPack on an invalid/already-sealed builder.
    ScriptPack seal() {
        if (!valid() || !fi_->builder_seal) return ScriptPack{};
        xi_pack_handle h = fi_->builder_seal(b_);
        b_ = XI_PACK_BUILDER_NULL;          // the builder id is dead either way
        if (h == XI_PACK_NULL) return ScriptPack{};
        auto keep = std::shared_ptr<SealRef>(new SealRef{h, fi_});
        return ScriptPack(h, fi_, std::move(keep));
    }

    // Drop a half-built pack explicitly (the destructor also does this): the
    // host releases anything the builder minted. Safe to call repeatedly.
    void abandon() {
        if (valid() && fi_->builder_abandon) fi_->builder_abandon(b_);
        b_ = XI_PACK_BUILDER_NULL;
    }

private:
    // The keepalive payload for a sealed script-built pack: its destructor —
    // run by the LAST ScriptPack copy's shared_ptr — drops the seal's host
    // refcount, exactly mirroring the trigger path where the Trigger's Data
    // destructor drops the retain() it took (xi_use.hpp).
    struct SealRef {
        xi_pack_handle    h;
        const xi_pack_v1* fi;
        ~SealRef() {
            if (h != XI_PACK_NULL && fi && fi->release) fi->release(h);
        }
    };

    const xi_pack_v1* fi_ = nullptr;
    xi_pack_builder   b_  = XI_PACK_BUILDER_NULL;
};

} // namespace xi
