#pragma once
//
// xi_kv.hpp — cross-frame script state, post-Record shape (U2 decision,
// docs/new_gen/16-script-state-shape.md).
//
// xi::kv() is the successor of xi::state(): a FLAT, TYPED, MUTABLE key-value
// store that lives entirely SDK-side (this header + xi_mp.hpp, nothing else)
// and crosses the host boundary only as one canonical max-width msgpack map
// (sorted keys — equal state serializes to equal bytes). During the bilingual
// window both xi::state() (Record/JSON channel) and xi::kv() (this, mp
// channel) are live; THE CUT deletes the Record channel and this remains.
//
//   #include <xi/xi.hpp>            // umbrella pulls this in
//
//   XI_KV_SCHEMA(1);                // optional: version the store's shape
//
//   XI_INSPECT_ENTRY(t, frame) {
//       std::lock_guard<std::mutex> lk(xi::kv_mutex());   // if using xi::async
//       long long n = xi::kv().get_i64("count", 0) + 1;
//       xi::kv().set_i64("count", n);
//   }
//
// Shape: typed scalar slots (i64 / f64 / bool / str / bin) plus an `mp` slot
// holding ONE complete canonical msgpack subtree (built with xi::mp::Writer,
// read back with xi::mp::Reader) for the rebuilt-each-frame structures
// (centroid lists, rolling windows). There is deliberately NO nested mutable
// document — that would be Record by another name.
//
// Canonical gate: set_mp() runs the bytes through xi::mp::canonicalize with
// the reject-all ext policy — the exact gate ScriptPackBuilder::add_mp
// applies — so the store can never hold non-canonical, ext-bearing,
// duplicate-keyed, or non-string-keyed bytes. parse() applies the same gate
// to the whole incoming buffer and adopts ALL-OR-NOTHING (a refused buffer
// leaves the store untouched).
//
// Thread safety: kv() returns a reference to a PROCESS-GLOBAL store, and the ONE
// inspect script DLL (g_eng.script) runs on EVERY dispatch lane. The host does not
// serialize inspect, so any inspect body that touches kv() MUST lock xi::kv_mutex()
// around each read-modify-write unless the TOTAL concurrency that can run the script
// is exactly 1. Total concurrency = Σ (per-group max_parallel) across all lanes,
// PLUS any xi::async / xi::parallel_for the body spawns — NOT a single group's
// max_parallel. So (G4 / M1):
//   - Multi-GROUP project (≥2 groups) — ALWAYS lock, even if every group is
//     max_parallel==1: the groups' lanes run the shared script concurrently.
//   - Single group with max_parallel>1, or ANY async/parallel_for — lock.
//   - ONLY safe to skip: a SINGLE-group project, max_parallel==1, no async — the
//     one configuration where the script is truly single-threaded across frames.
// A benign author who reads "my group is max_parallel==1, skip the lock" but runs a
// two-camera (two-group) project hits concurrent std::map RMW → torn count / red-black
// tree corruption. (The host-boundary thunks in xi::detail lock it themselves.)
//
// Hot-reload machinery mirrors the Record channel exactly:
//   XI_KV_SCHEMA(N) / xi::set_kv_schema_version(N)  — version the shape;
//     the host drops the persisted store on a version mismatch
//     (event:state_dropped with "store":"kv") instead of restoring a
//     mis-shaped store.
//   xi::set_kv_migrate(fn) — opt-in code_change-style migration: on a
//     mismatch the host hands the NEW DLL the old store (already parsed into
//     a Kv) and the old/new schema numbers; return the re-shaped Kv, or
//     std::nullopt to decline (host drops as usual).
//

#include "xi_mp.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xi {

class Kv {
public:
    enum class Type { I64, F64, Bool, Str, Bin, Mp };

    // ---- typed setters -----------------------------------------------------
    // Scalars always succeed (return true); set_bin/set_mp can refuse.
    bool set_i64(const std::string& key, int64_t v) {
        Slot s; s.t = Type::I64; s.i = v; m_[key] = std::move(s); return true;
    }
    bool set_f64(const std::string& key, double v) {
        Slot s; s.t = Type::F64; s.f = v; m_[key] = std::move(s); return true;
    }
    bool set_bool(const std::string& key, bool v) {
        Slot s; s.t = Type::Bool; s.b = v; m_[key] = std::move(s); return true;
    }
    bool set_str(const std::string& key, std::string_view v) {
        Slot s; s.t = Type::Str; s.s.assign(v.data(), v.size());
        m_[key] = std::move(s); return true;
    }
    bool set_bin(const std::string& key, const void* data, size_t n) {
        if (n && !data) return false;
        Slot s; s.t = Type::Bin;
        s.bytes.assign((const uint8_t*)data, (const uint8_t*)data + n);
        m_[key] = std::move(s); return true;
    }

    // ONE complete msgpack value (scalar / str / bin / map / array subtree).
    // THE CANONICAL GATE: bytes are validated AND normalized through
    // xi::mp::canonicalize (reject-all ext) before they are stored — canonical
    // input (an xi::mp::Writer value) passes byte-identical; malformed /
    // ext-bearing / non-string-keyed / duplicate-keyed bytes are REFUSED
    // (returns false, store untouched). A top-level bare Nil or a uint64
    // beyond INT64_MAX is ALSO refused — parse() rejects those value kinds,
    // so admitting one would sabotage the whole store's next round-trip.
    // Note: a SCALAR subtree stored here
    // parses back (after a host round-trip) as its typed slot, not as an Mp
    // slot — the bytes are identical either way; only type_of() differs.
    bool set_mp(const std::string& key, const void* mp_bytes, size_t n) {
        if (!mp_bytes || n == 0) return false;
        mp::Writer canon;
        if (mp::canonicalize((const uint8_t*)mp_bytes, n, canon) != mp::Status::Ok)
            return false;
        // Round-trip guard (matches parse()'s value rejections): parse()
        // refuses a top-level Nil and a uint64 beyond INT64_MAX, so storing
        // one here would poison serialize() output and drop the WHOLE store
        // at the next parse(). Refuse it now, fail-loud, store untouched.
        // Post-canonicalize, Kind::UInt appears ONLY for values > INT64_MAX
        // (uint_ encodes anything <= INT64_MAX as Int64), so kind alone is
        // the exact condition. Invariant: anything set_mp accepts, parse()
        // round-trips.
        {
            mp::Reader probe(canon.bytes().data(), canon.bytes().size());
            mp::Element top;
            if (probe.next(top) != mp::Status::Ok) return false;
            if (top.kind == mp::Kind::Nil || top.kind == mp::Kind::UInt)
                return false;
        }
        Slot s; s.t = Type::Mp; s.bytes = canon.take();
        m_[key] = std::move(s); return true;
    }
    bool set_mp(const std::string& key, const mp::Writer& w) {
        return set_mp(key, w.bytes().data(), w.bytes().size());
    }

    // ---- typed getters (default on absent / other type — Record precedent;
    //      has()/type_of() are the strict path) -------------------------------
    int64_t get_i64(const std::string& key, int64_t def = 0) const {
        const Slot* s = find_(key);
        return (s && s->t == Type::I64) ? s->i : def;
    }
    double get_f64(const std::string& key, double def = 0.0) const {
        const Slot* s = find_(key);
        if (!s) return def;
        if (s->t == Type::F64) return s->f;
        if (s->t == Type::I64) return (double)s->i;   // widening read is safe
        return def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        const Slot* s = find_(key);
        return (s && s->t == Type::Bool) ? s->b : def;
    }
    std::string get_str(const std::string& key, const std::string& def = "") const {
        const Slot* s = find_(key);
        return (s && s->t == Type::Str) ? s->s : def;
    }
    // Bin/Mp views: pointer into the store, valid until the entry is
    // overwritten/erased. Null when absent or a different type.
    const mp::Bytes* get_bin(const std::string& key) const {
        const Slot* s = find_(key);
        return (s && s->t == Type::Bin) ? &s->bytes : nullptr;
    }
    const mp::Bytes* get_mp(const std::string& key) const {
        const Slot* s = find_(key);
        return (s && s->t == Type::Mp) ? &s->bytes : nullptr;
    }

    bool has(const std::string& key) const { return m_.count(key) != 0; }
    // Type of an entry; only meaningful when has(key). Absent -> I64 is never
    // ambiguous in practice — check has() first (the strict path).
    Type type_of(const std::string& key) const {
        const Slot* s = find_(key);
        return s ? s->t : Type::I64;
    }
    bool erase(const std::string& key) { return m_.erase(key) != 0; }
    void clear() { m_.clear(); }
    size_t size() const { return m_.size(); }
    bool empty() const { return m_.empty(); }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(m_.size());
        for (auto& [k, _] : m_) out.push_back(k);
        return out;
    }

    // ---- serialization (the host-boundary format) ---------------------------
    // One canonical max-width msgpack map. Keys are emitted in std::map
    // (sorted) order, so equal stores serialize to EQUAL BYTES — the canonical
    // field-order duty (xi_mp.hpp header) is discharged by sorting.
    mp::Bytes serialize() const {
        mp::Writer w;
        w.map((uint32_t)m_.size());
        for (auto& [k, s] : m_) {
            w.key(k);
            switch (s.t) {
                case Type::I64:  w.int_(s.i); break;
                case Type::F64:  w.float_(s.f); break;
                case Type::Bool: w.boolean(s.b); break;
                case Type::Str:  w.str(s.s); break;
                case Type::Bin:  w.bin(s.bytes.data(), s.bytes.size()); break;
                case Type::Mp:   w.raw_canonical(s.bytes.data(), s.bytes.size()); break;
            }
        }
        return w.take();
    }

    // Adopt a serialized store. ALL-OR-NOTHING: on any refusal the current
    // contents are untouched and false is returned (fail-loud). Accepts
    // foreign compact widths (normalized through canonicalize — identity on
    // our own output); refuses ext, duplicate keys, non-string keys,
    // malformed/trailing bytes, a non-map top level, nil values, and uint
    // values beyond int64 range (our writer never emits them for a Kv).
    bool parse(const uint8_t* data, size_t len) {
        if (!data || len == 0) return false;
        mp::Writer canonw;
        if (mp::canonicalize(data, len, canonw) != mp::Status::Ok) return false;
        const mp::Bytes& canon = canonw.bytes();

        mp::Reader r(canon.data(), canon.size());
        mp::Element top;
        if (r.next(top) != mp::Status::Ok || top.kind != mp::Kind::Map) return false;

        std::map<std::string, Slot> out;
        for (uint32_t k = 0; k < top.len; ++k) {
            mp::Element key;
            if (r.next(key) != mp::Status::Ok || key.kind != mp::Kind::Str)
                return false;   // canonicalize enforces this already; belt+braces
            std::string kstr((const char*)key.data, key.len);

            size_t off = r.offset();
            mp::Element v;
            if (r.next(v) != mp::Status::Ok) return false;
            Slot s;
            switch (v.kind) {
                case mp::Kind::Int:   s.t = Type::I64;  s.i = v.i; break;
                case mp::Kind::Bool:  s.t = Type::Bool; s.b = v.b; break;
                case mp::Kind::Float: s.t = Type::F64;  s.f = v.d; break;
                case mp::Kind::Str:   s.t = Type::Str;
                                      s.s.assign((const char*)v.data, v.len); break;
                case mp::Kind::Bin:   s.t = Type::Bin;
                                      s.bytes.assign(v.data, v.data + v.len); break;
                case mp::Kind::Array:
                case mp::Kind::Map: {
                    // Capture the WHOLE canonical subtree (header already
                    // consumed; skip the children, then slice).
                    uint32_t pairs = v.len;
                    bool is_map = (v.kind == mp::Kind::Map);
                    for (uint32_t c = 0; c < pairs; ++c) {
                        if (is_map && !skip_value_(r)) return false;   // key
                        if (!skip_value_(r)) return false;             // value/elem
                    }
                    s.t = Type::Mp;
                    s.bytes.assign(canon.data() + off, canon.data() + r.offset());
                    break;
                }
                case mp::Kind::UInt:  return false;  // beyond int64: never ours (set_mp refuses these at store time)
                case mp::Kind::Nil:   return false;  // never emitted for a Kv (set_mp refuses these at store time)
                case mp::Kind::Ext:   return false;  // canonicalize rejected already
            }
            out[std::move(kstr)] = std::move(s);
        }
        // canonicalize() already rejected trailing bytes at the top level.
        m_ = std::move(out);
        return true;
    }

    // Human-readable view for logs/debugging ONLY — never a boundary format.
    std::string debug_text() const {
        std::string out = "{";
        bool first = true;
        for (auto& [k, s] : m_) {
            if (!first) out += ",";
            first = false;
            out += "\"" + k + "\":";
            switch (s.t) {
                case Type::I64:  out += std::to_string(s.i); break;
                case Type::F64:  out += std::to_string(s.f); break;
                case Type::Bool: out += s.b ? "true" : "false"; break;
                case Type::Str:  out += "\"" + s.s + "\""; break;
                case Type::Bin:  out += "<bin " + std::to_string(s.bytes.size()) + ">"; break;
                case Type::Mp:   out += "<mp "  + std::to_string(s.bytes.size()) + ">"; break;
            }
        }
        out += "}";
        return out;
    }

private:
    struct Slot {
        Type        t = Type::I64;
        int64_t     i = 0;
        double      f = 0.0;
        bool        b = false;
        std::string s;
        mp::Bytes   bytes;   // Bin payload, or one canonical Mp subtree
    };

    const Slot* find_(const std::string& key) const {
        auto it = m_.find(key);
        return it != m_.end() ? &it->second : nullptr;
    }

    // Structurally consume ONE value at the cursor (any kind). The input is
    // already canonicalize-validated, so this cannot loop past the buffer.
    static bool skip_value_(mp::Reader& r) {
        mp::Element e;
        if (r.next(e) != mp::Status::Ok) return false;
        if (e.kind == mp::Kind::Array) {
            for (uint32_t k = 0; k < e.len; ++k)
                if (!skip_value_(r)) return false;
        } else if (e.kind == mp::Kind::Map) {
            for (uint32_t k = 0; k < e.len; ++k) {
                if (!skip_value_(r)) return false;   // key
                if (!skip_value_(r)) return false;   // value
            }
        }
        return true;   // scalars/str/bin/ext: next() consumed the payload
    }

    std::map<std::string, Slot> m_;
};

// ---- the store + its mutex (same discipline as xi::state()) ----------------

inline Kv& kv() {
    static Kv s;
    return s;
}

inline std::mutex& kv_mutex() {
    static std::mutex mu;
    return mu;
}

// ---- schema version (mirror of state_schema_version) -----------------------
//
// Bump when the SHAPE of the store changes (key renamed, type changed, meaning
// changed) — the host drops the persisted store on a version mismatch
// (event:state_dropped, "store":"kv") instead of restoring a mis-shaped one.
// Default 0 = unversioned: the host restores blindly (best-effort).
// Register at static-init time: XI_KV_SCHEMA(2); (macro below).
inline std::atomic<int>& kv_schema_version_ref() {
    static std::atomic<int> v{0};
    return v;
}
inline int  kv_schema_version()          { return kv_schema_version_ref().load(std::memory_order_relaxed); }
inline void set_kv_schema_version(int v) { kv_schema_version_ref().store(v, std::memory_order_relaxed); }

// ---- opt-in migration hook (typed code_change) ------------------------------
//
// On a schema mismatch the host, BEFORE dropping, hands the NEW DLL the old
// store (parsed) and both schema numbers; return the re-shaped Kv to carry it
// forward, or std::nullopt to decline (host drops as usual — the pre-hook
// behaviour). Runs in the NEW DLL, which alone knows both shapes. Must be pure
// w.r.t. the live xi::kv() (not restored yet). An unreadable old buffer or an
// EMPTY returned Kv also results in the drop path.
using KvMigrateFn = std::function<std::optional<Kv>(const Kv& old, int old_schema, int new_schema)>;

inline KvMigrateFn& kv_migrate_fn() {
    static KvMigrateFn f;
    return f;
}
inline void set_kv_migrate(KvMigrateFn f) { kv_migrate_fn() = std::move(f); }

// ---- host-boundary thunk BODIES ---------------------------------------------
//
// The real script exports in xi_script_support.hpp are one-line wrappers over
// these, and the loader-level test fixture (kv_probe.cpp) exports the SAME
// bodies — so tests exercise the exact production logic without force-
// including the whole script world. Convention: byte lengths, NO NUL
// termination (msgpack bytes contain NULs); grow-and-retry via a negative
// "-needed" return, exactly like the Record channel's thunks.
namespace detail {

// >0 bytes written; -N buffer too small (need N); 0 = store EMPTY (the host
// then stores nothing — the explicit contract replacing the Record channel's
// `size() > 2` emptiness heuristic).
inline int kv_get_thunk(uint8_t* buf, int buflen) {
    std::lock_guard<std::mutex> lk(kv_mutex());
    if (kv().empty()) return 0;
    mp::Bytes b = kv().serialize();
    int needed = (int)b.size();
    if (!buf || buflen < needed) return -needed;
    std::memcpy(buf, b.data(), b.size());
    return needed;
}

// 0 adopted; -1 refused (invalid bytes — store left untouched, fail-loud).
inline int kv_set_thunk(const uint8_t* bytes, int len) {
    if (!bytes || len <= 0) return -1;
    std::lock_guard<std::mutex> lk(kv_mutex());
    return kv().parse(bytes, (size_t)len) ? 0 : -1;
}

// 0 = decline (no migrator / migrator declined / old bytes unreadable /
// migrated store empty) -> host drops; >0/-N migrated bytes (grow-and-retry).
inline int kv_change_thunk(const uint8_t* old_bytes, int old_len,
                           int old_schema, int new_schema,
                           uint8_t* buf, int buflen) {
    auto& fn = kv_migrate_fn();
    if (!fn) return 0;
    Kv old;
    if (old_bytes && old_len > 0 && !old.parse(old_bytes, (size_t)old_len))
        return 0;   // unreadable prior store -> decline -> host drops
    std::optional<Kv> migrated = fn(old, old_schema, new_schema);
    if (!migrated || migrated->empty()) return 0;
    mp::Bytes b = migrated->serialize();
    int needed = (int)b.size();
    if (!buf || buflen < needed) return -needed;
    std::memcpy(buf, b.data(), b.size());
    return needed;
}

} // namespace detail

} // namespace xi

// Convenience macro: register the schema version once at DLL load (file
// scope). Same pattern (and same /FI-ordering rationale) as XI_STATE_SCHEMA.
#define XI_KV_SCHEMA(N) \
    namespace { static int _xi_kv_schema_init_##N = (xi::set_kv_schema_version(N), 0); }
