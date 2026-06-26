#pragma once
//
// xi_record.hpp — unified output type for inspection steps.
//
// A Record bundles named images + schemaless JSON data. Backed by yyjson
// (a mutable doc) so building is incremental + mutable and reading/serializing
// are fast; escaping, nesting, and serialization are handled correctly.
// (Migrated from cJSON 2026-06 — see docs/internals/data-layer.md.)
//
// Usage:
//
//   VAR(result, xi::Record()
//       .image("input", img)
//       .image("edges", edge_img)
//       .set("count", 5)
//       .set("pass", true)
//       .set("details", xi::Record()
//           .set("area", 142.5)
//           .set("label", "ok")));
//
// yyjson note: keys and string values are BORROWED by the mutable API, so every
// key/string we store goes through yyjson_mut_strcpy (copy into the doc) — the
// std::string callers pass is transient.
//

#include "xi_image.hpp"
#include "yyjson.h"

#include <atomic>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>

namespace xi {

// Non-finite doubles can't be represented in standard JSON. The vendored yyjson
// writer emits the bare tokens `NaN`/`Infinity` (non-standard) — which our reader
// and JS `JSON.parse` REJECT, so a single non-finite field would make the parse
// of the WHOLE record fail and drop every field. We store them as explicit
// quoted string sentinels instead; these helpers convert both ways so a
// non-finite value survives the Record↔JSON round-trip and stays visible. Every
// double-write path (set/push, the typed layer's mut_finite_) must route through
// these — bypassing them re-opens the whole-frame-drop hazard.
inline const char* nonfinite_to_str(double v) {
    if (std::isnan(v)) return "NaN";
    return v > 0 ? "Infinity" : "-Infinity";
}
inline bool nonfinite_from_str(const char* s, double& out) {
    if (!s) return false;
    if (std::strcmp(s, "NaN") == 0)       { out = std::numeric_limits<double>::quiet_NaN(); return true; }
    if (std::strcmp(s, "Infinity") == 0)  { out = std::numeric_limits<double>::infinity();  return true; }
    if (std::strcmp(s, "-Infinity") == 0) { out = -std::numeric_limits<double>::infinity(); return true; }
    return false;
}

// Minimal JSON string escape — kept local so this header stays independent
// of xi_protocol.hpp. Emits the value already wrapped in quotes.
inline void append_json_escaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// yyjson layout stamp (γ). Identifies the yyjson version + the two struct sizes
// the in-process doc-pointer path dereferences. The host passes a raw
// yyjson_mut_doc* across the ABI ONLY when a plugin's exported xi_yyjson_abi()
// equals the host's own stamp — so a plugin carrying a different yyjson build
// transparently falls back to the JSON path instead of walking an incompatible
// struct. Both the host and the XI_PLUGIN_IMPL export call this one function.
inline uint32_t yyjson_layout_stamp() {
    return (uint32_t)YYJSON_VERSION_HEX
         ^ ((uint32_t)sizeof(yyjson_mut_doc) << 8)
         ^ ((uint32_t)sizeof(yyjson_mut_val) << 18);
}

// Thread-local doc allocator (γ). When non-null, every new Record doc is built
// from it — the host doc pool, installed by the in-process call seam — so the
// doc is host-owned and safe to hand across the ABI (its free routes back to the
// host via doc->alc). Null (the default, standalone use) ⇒ yyjson's default
// allocator, identical to the pre-γ behaviour. yyjson copies the alc into each
// doc, so a pointer to a transient alc is fine.
inline const yyjson_alc*& tls_doc_alc() {
    static thread_local const yyjson_alc* a = nullptr;
    return a;
}

class Record {
private:
    // γ-4: intrusive refcount box around an owned doc. Multiple Records can share
    // one DocBox (copy = ref bump, zero deep-copy); a mutation copy-on-writes into
    // a fresh sole-owned box. box_ == nullptr marks a BORROWED read-only view
    // (from_doc_view): the doc is owned elsewhere across the ABI, never freed here.
    //
    // host_release (γ-4 v4): when non-null, this doc is REGISTRY-MANAGED — it is
    // shared across the ABI and the host's DocRegistry holds the authoritative
    // refcount. The last IN-PROCESS ref then frees via host_release(doc) (= host
    // doc_release) rather than yyjson_mut_doc_free, so the doc lives until the
    // last SIDE drops it. null ⇒ a plain locally-owned doc, freed directly.
    struct DocBox { yyjson_mut_doc* doc; std::atomic<int> rc; void (*host_release)(void*); };
    static DocBox* new_box_(yyjson_mut_doc* d) { return new DocBox{ d, 1, nullptr }; }
    void release_box_() {
        if (box_ && box_->rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (box_->doc) {
                if (box_->host_release) box_->host_release(box_->doc); // registry decides the real free
                else                    yyjson_mut_doc_free(box_->doc);
            }
            delete box_;
        }
        box_ = nullptr;
    }

public:
    Record() { init_(); }

    ~Record() { release_box_(); }

    Record(const Record& o) : images_(o.images_) {
        if (o.box_) {
            // Share the doc — zero copy. Both copies are now FROZEN (read-only);
            // the first mutation on either copy-on-writes into its own doc.
            box_ = o.box_;
            box_->rc.fetch_add(1, std::memory_order_relaxed);
            doc_ = o.doc_; root_ = o.root_;
            o.frozen_ = true; frozen_ = true;
        } else {
            // o is a borrowed view (no owned box) → make an independent owned
            // copy (reading o.root_ is safe; we allocate our own).
            doc_  = yyjson_mut_doc_new(tls_doc_alc());
            root_ = o.root_ ? yyjson_mut_val_mut_copy(doc_, o.root_) : yyjson_mut_obj(doc_);
            yyjson_mut_doc_set_root(doc_, root_);
            box_ = new_box_(doc_);
        }
    }

    Record& operator=(const Record& o) {
        if (this != &o) {
            images_ = o.images_;
            release_box_();
            if (o.box_) {
                box_ = o.box_;
                box_->rc.fetch_add(1, std::memory_order_relaxed);
                doc_ = o.doc_; root_ = o.root_;
                o.frozen_ = true; frozen_ = true;
            } else {
                doc_  = yyjson_mut_doc_new(tls_doc_alc());
                root_ = o.root_ ? yyjson_mut_val_mut_copy(doc_, o.root_) : yyjson_mut_obj(doc_);
                yyjson_mut_doc_set_root(doc_, root_);
                box_ = new_box_(doc_);
                frozen_ = false;
            }
        }
        return *this;
    }

    Record(Record&& o) noexcept
        : images_(std::move(o.images_)), doc_(o.doc_), root_(o.root_),
          box_(o.box_), frozen_(o.frozen_) {
        o.doc_ = nullptr; o.root_ = nullptr; o.box_ = nullptr;
    }

    Record& operator=(Record&& o) noexcept {
        if (this != &o) {
            images_ = std::move(o.images_);
            release_box_();
            doc_ = o.doc_; root_ = o.root_; box_ = o.box_; frozen_ = o.frozen_;
            o.doc_ = nullptr; o.root_ = nullptr; o.box_ = nullptr;
        }
        return *this;
    }

    // --- γ-4 v4: cross-ABI shared doc (host-registry refcounted) -------------
    //
    // The doc analogue of an image pool handle: a doc crossing the ABI is owned by
    // the host DocRegistry and held by refcount, NOT borrowed — so either side
    // (producer or consumer) can cache it across frames with zero copy, and it
    // lives until the last side releases. Both input and output use this; there is
    // no borrowed-view special case.
    //
    // share_out: enroll this Record's owned doc into the host registry and return
    // the raw pointer for the other side to adopt_shared(). The doc becomes frozen
    // (shared ⇒ read-only; a later mutation COWs into a fresh owned doc) and its
    // box becomes registry-managed (free routes through host doc_release). We take
    // exactly ONE registry ref — for THIS side; the adopter takes its own in
    // adopt_shared. So if the other side never adopts (e.g. it took the JSON
    // path), nothing leaks. `retain`/`release` are the host_api doc_* (both must
    // be non-null). const — it changes only refcount/freeze metadata, so it works
    // on a borrowed input being handed straight on. Returns null when there's no
    // owned doc (moved-from); the caller then serializes.
    yyjson_mut_doc* share_out(void (*retain)(void*), void (*release)(void*)) const {
        if (!box_ || !box_->doc || !retain || !release) return nullptr;
        frozen_ = true;                       // shared ⇒ read-only from here on
        if (!box_->host_release) {            // first cross-ABI share: enroll this side
            retain(box_->doc);                // registry rc += 1 (this side's ref)
            box_->host_release = release;     // our box now frees via the registry
        }
        retain(box_->doc);                    // +1 RESERVED for the adopting side. This
                                              // keeps the doc alive until adopt_shared
                                              // consumes it — the producer's own Record
                                              // may die BEFORE the other side adopts (e.g.
                                              // a plugin's output Record is destroyed when
                                              // process() returns, the host adopts after).
                                              // If the other side never adopts (JSON path),
                                              // the caller that took the JSON branch must
                                              // doc_release once to balance this reserve.
        return box_->doc;
    }

    // adopt_shared: the receiving side of share_out. It does NOT retain — it CONSUMES
    // the ref share_out reserved for us, wrapping the doc in a registry-managed box;
    // the last in-process ref here calls release (host doc_release), and the doc lives
    // until the last SIDE releases.
    //
    // `frozen`: pass false when WE are the sole side (common dispatch case — no one
    // else reads it, so mutate in place, zero COW, write-through intact); true when
    // another side still holds it (the producer cached the same doc) so the first
    // mutation copy-on-writes to keep them isolated.
    static Record adopt_shared(yyjson_mut_doc* doc, void (*release)(void*),
                               bool frozen = true) {
        Record r;
        if (doc && release) {
            r.release_box_();                 // drop the fresh init_ box
            r.box_  = new DocBox{ doc, 1, release };  // consume share_out's reserved ref
            r.doc_  = doc;
            r.root_ = yyjson_mut_doc_get_root(doc);
            r.frozen_ = frozen;
        }
        return r;
    }

    // --- Image builder ---
    Record& image(const std::string& key, Image img) {
        images_[key] = std::move(img);
        return *this;
    }

    // --- Data builders (add to the JSON object) ---
    //
    // Integer overload accepts every standard integral type (int, long, int64_t,
    // size_t, ...). Stored as a JSON integer; bool is excluded so it goes to the
    // dedicated bool overload below.
    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> &&
                                          !std::is_same_v<T, bool>>>
    Record& set(const std::string& key, T v) {
        cow_();
        put_(key.c_str(), yyjson_mut_sint(doc_, (int64_t)v));
        return *this;
    }
    Record& set(const std::string& key, double v) {
        cow_();
        if (!std::isfinite(v))
            put_(key.c_str(), yyjson_mut_strcpy(doc_, nonfinite_to_str(v)));
        else
            put_(key.c_str(), yyjson_mut_real(doc_, v));
        return *this;
    }
    Record& set(const std::string& key, float v) { return set(key, (double)v); }
    Record& set(const std::string& key, bool v) {
        cow_();
        put_(key.c_str(), yyjson_mut_bool(doc_, v));
        return *this;
    }
    Record& set(const std::string& key, const std::string& v) {
        cow_();
        put_(key.c_str(), yyjson_mut_strcpy(doc_, v.c_str()));
        return *this;
    }
    Record& set(const std::string& key, const char* v) { return set(key, std::string(v ? v : "")); }

    // Nest a sub-Record as a JSON object (deep copy into this doc).
    // KNOWN LIMITATION: only the JSON is copied, NOT the sub-Record's images_ —
    // so nesting a Record that carries images (e.g. xi::Region's mask) silently
    // drops those pixels. Warn once so an author hits it loudly instead of
    // chasing an empty mask. (Proper fix = namespacing sub images into the
    // parent; deferred.)
    Record& set(const std::string& key, const Record& sub) {
        cow_();
        warn_sub_images_dropped_(sub, key.c_str());
        yyjson_mut_val* dup = sub.root_ ? yyjson_mut_val_mut_copy(doc_, sub.root_)
                                        : yyjson_mut_obj(doc_);
        put_(key.c_str(), dup);
        return *this;
    }

    // Add a raw yyjson value that ALREADY belongs to this Record's doc (advanced).
    // NOTE: not valid on a frozen/borrowed view — `item` must come from THIS
    // Record's (owned) doc, so callers building `item` must already have an owned
    // doc; cow_() here would orphan an item built against the pre-COW doc.
    // A frozen Record shares its doc with another copy, so writing here would
    // corrupt that sibling — assert against it in debug (compiled out in release,
    // zero hot-path cost). Force ownership first by mutating via a normal setter.
    Record& set_raw(const std::string& key, yyjson_mut_val* item) {
        assert(!frozen_ && "set_raw on a frozen/shared Record corrupts the sibling copy "
                           "— mutate it through a normal setter first to force copy-on-write");
        put_(key.c_str(), item);
        return *this;
    }

    // --- Array builders ---
    Record& push(const std::string& key, int v) {
        cow_();
        yyjson_mut_arr_add_val(ensure_arr_(key.c_str()), yyjson_mut_sint(doc_, v));
        return *this;
    }
    Record& push(const std::string& key, double v) {
        cow_();
        // Same non-finite guard as set(double): a bare NaN/Infinity token is
        // invalid JSON and would make the consumer's parse of the WHOLE record
        // fail (dropping every field), so emit the quoted sentinel instead.
        yyjson_mut_val* val = std::isfinite(v)
            ? yyjson_mut_real(doc_, v)
            : yyjson_mut_strcpy(doc_, nonfinite_to_str(v));
        yyjson_mut_arr_add_val(ensure_arr_(key.c_str()), val);
        return *this;
    }
    Record& push(const std::string& key, bool v) {
        cow_();
        yyjson_mut_arr_add_val(ensure_arr_(key.c_str()), yyjson_mut_bool(doc_, v));
        return *this;
    }
    Record& push(const std::string& key, const std::string& v) {
        cow_();
        yyjson_mut_arr_add_val(ensure_arr_(key.c_str()), yyjson_mut_strcpy(doc_, v.c_str()));
        return *this;
    }
    Record& push(const std::string& key, const Record& sub) {
        cow_();
        warn_sub_images_dropped_(sub, key.c_str());   // see set(key, Record&)
        yyjson_mut_val* dup = sub.root_ ? yyjson_mut_val_mut_copy(doc_, sub.root_)
                                        : yyjson_mut_obj(doc_);
        yyjson_mut_arr_add_val(ensure_arr_(key.c_str()), dup);
        return *this;
    }

    // --- Proxy for chained [] access ---
    //
    //   rec["roi"]["x"].as_int(0)
    //   rec["points"][2]["score"].as_double()
    //   rec["config"]["mode"].as_string("auto")
    //   rec["items"][0].as_record()
    //
    class Value {
    public:
        Value() : node_(nullptr) {}
        explicit Value(yyjson_mut_val* node) : node_(node) {}

        Value operator[](const char* key) const {
            if (!node_ || !yyjson_mut_is_obj(node_)) return {};
            return Value(yyjson_mut_obj_get(node_, key));
        }
        Value operator[](const std::string& key) const { return (*this)[key.c_str()]; }

        Value operator[](int index) const {
            if (!node_ || !yyjson_mut_is_arr(node_)) return {};
            return Value(yyjson_mut_arr_get(node_, (size_t)index));
        }

        // Terminal reads with defaults
        int    as_int(int def = 0) const {
            if (!is_number()) return def;
            double d = yyjson_mut_get_num(node_);
            // Clamp before the cast — (int)1e300 / (int)NaN is UB, and a value
            // past INT range would silently wrap. Out-of-range falls back to def.
            if (!(d >= (double)INT_MIN && d <= (double)INT_MAX)) return def;
            return (int)d;
        }
        // Full-width integer read (no int32 narrowing) for counts/ids that may
        // exceed 2^31. as_int() truncates the fractional part and rejects
        // out-of-int-range values; use this when the value is a large integer.
        long long as_int64(long long def = 0) const {
            if (!is_number()) return def;
            double d = yyjson_mut_get_num(node_);
            // Upper bound is STRICT: 9.2233720368547758e18 rounds to exactly 2^63
            // (LLONG_MAX = 2^63-1 is not representable as double), and (long long)(2^63)
            // is itself UB. -2^63 = LLONG_MIN is representable, so the lower bound keeps >=.
            if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) return def;
            return (long long)d;
        }
        double as_double(double def = 0.0) const {
            if (is_number()) return yyjson_mut_get_num(node_);
            if (node_ && yyjson_mut_is_str(node_)) { double v; if (nonfinite_from_str(yyjson_mut_get_str(node_), v)) return v; }
            return def;
        }
        // Consistent with as_int/as_double: a present-but-non-bool falls back to
        // `def` (not silently false), and a number / "true"/"false" string is
        // coerced so a value from an external/PLC JSON isn't misread.
        bool as_bool(bool def = false) const {
            if (!node_) return def;
            if (yyjson_mut_is_bool(node_)) return yyjson_mut_get_bool(node_);
            if (yyjson_mut_is_num(node_))  return yyjson_mut_get_num(node_) != 0.0;
            if (yyjson_mut_is_str(node_)) {
                const char* s = yyjson_mut_get_str(node_);
                if (s && (!std::strcmp(s, "true")  || !std::strcmp(s, "1"))) return true;
                if (s && (!std::strcmp(s, "false") || !std::strcmp(s, "0"))) return false;
            }
            return def;
        }
        std::string as_string(const std::string& def = "") const {
            const char* s = (node_ && yyjson_mut_is_str(node_)) ? yyjson_mut_get_str(node_) : nullptr;
            return s ? std::string(s) : def;
        }

        int size() const { return (node_ && yyjson_mut_is_arr(node_)) ? (int)yyjson_mut_arr_size(node_) : 0; }

        bool exists()    const { return node_ != nullptr; }
        bool is_null()   const { return !node_ || yyjson_mut_is_null(node_); }
        bool is_object() const { return node_ && yyjson_mut_is_obj(node_); }
        bool is_array()  const { return node_ && yyjson_mut_is_arr(node_); }
        bool is_number() const { return node_ && yyjson_mut_is_num(node_); }
        bool is_string() const { return node_ && yyjson_mut_is_str(node_); }
        bool is_bool()   const { return node_ && yyjson_mut_is_bool(node_); }

        // Extract as a standalone Record (deep copy).
        Record as_record() const {
            if (!node_ || !yyjson_mut_is_obj(node_)) return {};
            return Record(node_);   // private deep-copy ctor
        }

        // Raw yyjson value (advanced; for cross-doc copy via set_value or
        // iteration). Belongs to the source Record's doc — do not free.
        yyjson_mut_val* raw() const { return node_; }

        // Path expression access:
        //   val.at(".a.b[3].c")     object→object→array[3]→object
        //   val.at("[1].x")         array[1]→object
        //   val.at("a.b.c")         same as .a.b.c
        Value at(const char* path) const { return Value(resolve_path(node_, path)); }
        Value at(const std::string& path) const { return at(path.c_str()); }

    private:
        yyjson_mut_val* node_;

        static yyjson_mut_val* resolve_path(yyjson_mut_val* root, const char* p) {
            if (!root || !p) return nullptr;
            yyjson_mut_val* cur = root;
            while (*p && cur) {
                if (*p == '.') ++p;  // skip leading or separator dot
                if (*p == '[') {
                    ++p;
                    if (*p < '0' || *p > '9') return nullptr;
                    int idx = 0;
                    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); ++p; }
                    if (*p != ']') return nullptr;
                    ++p;
                    if (!yyjson_mut_is_arr(cur)) return nullptr;
                    cur = yyjson_mut_arr_get(cur, (size_t)idx);
                } else {
                    const char* start = p;
                    while (*p && *p != '.' && *p != '[') ++p;
                    if (p == start) return nullptr;
                    std::string key(start, p - start);
                    if (!yyjson_mut_is_obj(cur)) return nullptr;
                    cur = yyjson_mut_obj_get(cur, key.c_str());
                }
            }
            return cur;
        }
    };

    // Entry point for chained access
    Value operator[](const char* key) const {
        bool is_path = false;
        for (const char* p = key; *p; ++p) {
            if (*p == '.' || *p == '[') { is_path = true; break; }
        }
        if (is_path) return Value(root_).at(key);
        return Value(root_ ? yyjson_mut_obj_get(root_, key) : nullptr);
    }
    Value operator[](const std::string& key) const { return (*this)[key.c_str()]; }
    Value at(const char* path) const { return Value(root_).at(path); }
    Value at(const std::string& path) const { return at(path.c_str()); }

    // Deep-copy any Value (from any Record) into this Record under `key`.
    // The ergonomic replacement for the old set_raw(duplicate(...)) idiom.
    Record& set_value(const std::string& key, const Value& v) {
        cow_();
        yyjson_mut_val* src = v.raw();
        put_(key.c_str(), src ? yyjson_mut_val_mut_copy(doc_, src) : yyjson_mut_null(doc_));
        return *this;
    }

    // --- Data getters (with defaults) ---
    // These mirror Value::as_int/as_int64/as_bool exactly (range-clamped int,
    // present-but-non-bool -> def with number/"true" coercion).
    int get_int(const std::string& key, int def = 0) const {
        yyjson_mut_val* it = get_(key.c_str());
        if (!it || !yyjson_mut_is_num(it)) return def;
        double d = yyjson_mut_get_num(it);
        if (!(d >= (double)INT_MIN && d <= (double)INT_MAX)) return def;
        return (int)d;
    }
    long long get_int64(const std::string& key, long long def = 0) const {
        yyjson_mut_val* it = get_(key.c_str());
        if (!it || !yyjson_mut_is_num(it)) return def;
        double d = yyjson_mut_get_num(it);
        // Strict upper bound: 9.2233720368547758e18 == 2^63 as double; (long long)(2^63) is UB.
        if (!(d >= -9.2233720368547758e18 && d < 9.2233720368547758e18)) return def;
        return (long long)d;
    }
    double get_double(const std::string& key, double def = 0.0) const {
        yyjson_mut_val* it = get_(key.c_str());
        if (it && yyjson_mut_is_num(it)) return yyjson_mut_get_num(it);
        if (it && yyjson_mut_is_str(it)) { double v; if (nonfinite_from_str(yyjson_mut_get_str(it), v)) return v; }
        return def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        yyjson_mut_val* it = get_(key.c_str());
        if (!it) return def;
        if (yyjson_mut_is_bool(it)) return yyjson_mut_get_bool(it);
        if (yyjson_mut_is_num(it))  return yyjson_mut_get_num(it) != 0.0;
        if (yyjson_mut_is_str(it)) {
            const char* s = yyjson_mut_get_str(it);
            if (s && (!std::strcmp(s, "true")  || !std::strcmp(s, "1"))) return true;
            if (s && (!std::strcmp(s, "false") || !std::strcmp(s, "0"))) return false;
        }
        return def;
    }
    std::string get_string(const std::string& key, const std::string& def = "") const {
        yyjson_mut_val* it = get_(key.c_str());
        const char* s = (it && yyjson_mut_is_str(it)) ? yyjson_mut_get_str(it) : nullptr;
        return s ? std::string(s) : def;
    }
    bool has(const std::string& key) const { return get_(key.c_str()) != nullptr; }

    // --- NA (not-available) -------------------------------------------------
    static constexpr const char* kNaKey = "$na";
    static Record na(const std::string& reason = "") {
        Record r;
        r.set(kNaKey, reason);
        return r;
    }
    bool is_na() const { return has(kNaKey); }
    std::string na_reason() const { return get_string(kNaKey); }

    // --- Provenance (src id) ------------------------------------------------
    static constexpr const char* kSrcKey  = "$src";
    static constexpr const char* kProvKey = "$prov";
    Record& set_src(const std::string& id) { return set(kSrcKey, id); }
    std::string src() const { return get_string(kSrcKey); }
    Record& set_prov(const std::string& field, const std::string& src) {
        if (src.empty()) return *this;
        cow_();
        yyjson_mut_val* prov = get_(kProvKey);
        if (!prov || !yyjson_mut_is_obj(prov)) {
            prov = yyjson_mut_obj(doc_);
            put_(kProvKey, prov);
        }
        yyjson_mut_obj_remove_key(prov, field.c_str());
        yyjson_mut_obj_put(prov, yyjson_mut_strcpy(doc_, field.c_str()),
                                 yyjson_mut_strcpy(doc_, src.c_str()));
        return *this;
    }
    std::string prov_of(const std::string& field) const {
        yyjson_mut_val* prov = get_(kProvKey);
        if (!prov || !yyjson_mut_is_obj(prov)) return "";
        yyjson_mut_val* f = yyjson_mut_obj_get(prov, field.c_str());
        const char* s = (f && yyjson_mut_is_str(f)) ? yyjson_mut_get_str(f) : nullptr;
        return s ? std::string(s) : "";
    }

    // Get a nested Record (returns empty Record if not found).
    Record get_record(const std::string& key) const {
        yyjson_mut_val* it = get_(key.c_str());
        if (!it || !yyjson_mut_is_obj(it)) return {};
        return Record(it);
    }
    int get_array_size(const std::string& key) const {
        yyjson_mut_val* it = get_(key.c_str());
        return (it && yyjson_mut_is_arr(it)) ? (int)yyjson_mut_arr_size(it) : 0;
    }
    Record get_array_item(const std::string& key, int index) const {
        yyjson_mut_val* arr = get_(key.c_str());
        if (!arr || !yyjson_mut_is_arr(arr)) return {};
        yyjson_mut_val* it = yyjson_mut_arr_get(arr, (size_t)index);
        if (!it || !yyjson_mut_is_obj(it)) return {};
        return Record(it);
    }

    // --- Image accessors ---
    const std::map<std::string, Image>& images() const { return images_; }
    // The mutable yyjson root object (advanced; used by the ABI seam).
    yyjson_mut_val* json() const { return root_; }
    yyjson_mut_doc* doc()  const { return doc_; }
    bool has_image(const std::string& key) const { return images_.count(key) > 0; }
    const Image& get_image(const std::string& key) const {
        static const Image empty;
        auto it = images_.find(key);
        return it != images_.end() ? it->second : empty;
    }
    Image get_image(const std::string& key, const Image& def) const {
        auto it = images_.find(key);
        return it != images_.end() ? it->second : def;
    }

    // --- Serialization ---
    std::string data_json() const {
        if (!doc_) return "{}";
        size_t len = 0;
        char* s = yyjson_mut_write(doc_, 0, &len);
        if (!s) return "{}";
        std::string out(s, len);
        free(s);
        return out;
    }
    std::string data_json_pretty() const {
        if (!doc_) return "{}";
        size_t len = 0;
        char* s = yyjson_mut_write(doc_, YYJSON_WRITE_PRETTY, &len);
        if (!s) return "{}";
        std::string out(s, len);
        free(s);
        return out;
    }

    // Build a Record from a JSON-text buffer (the wire decode path). Parses with
    // yyjson (immutable, fast) and materialises a mutable Record. Total: returns
    // an empty Record on null / empty / malformed / non-object input.
    static Record from_json_bytes(const uint8_t* data, size_t len) {
        Record r;
        if (!data || len == 0) return r;
        yyjson_doc* idoc = yyjson_read((const char*)data, len, 0);
        if (!idoc) return r;
        yyjson_val* iroot = yyjson_doc_get_root(idoc);
        if (iroot && yyjson_is_obj(iroot)) {
            r.release_box_();          // drop the fresh init_ box (frees its doc)
            r.doc_  = yyjson_mut_doc_new(tls_doc_alc());
            r.root_ = yyjson_val_mut_copy(r.doc_, iroot);
            yyjson_mut_doc_set_root(r.doc_, r.root_);
            r.box_  = new_box_(r.doc_);
        }
        yyjson_doc_free(idoc);
        return r;
    }

    std::string image_keys_json() const {
        std::string out = "[";
        bool first = true;
        for (auto& [k, _] : images_) {
            if (!first) out += ",";
            first = false;
            append_json_escaped(out, k);
        }
        out += "]";
        return out;
    }

    bool empty() const {
        return images_.empty() && (!root_ || yyjson_mut_obj_size(root_) == 0);
    }

private:
    std::map<std::string, Image> images_;
    yyjson_mut_doc* doc_  = nullptr;
    yyjson_mut_val* root_ = nullptr;
    // γ-4 ownership. box_: the intrusive refcount box owning doc_ (shared across
    // copies). nullptr ⇒ a BORROWED read-only view (doc owned elsewhere across
    // the ABI; never freed here). frozen_ (mutable — set on the const source when
    // a copy shares it): doc_ is shared/borrowed read-only → the first mutation
    // copy-on-writes (cow_) into a fresh sole-owned doc. Defaults give every
    // construction path a sole-owned, writable box, exactly as before.
    DocBox*      box_ = nullptr;
    mutable bool frozen_ = false;

    // One-shot warning when a sub-Record carrying images is nested (its images_
    // are dropped — see set(key, Record&)). Static flag ⇒ at most one line per
    // process per TU, never on the hot path for image-less records.
    static void warn_sub_images_dropped_(const Record& sub, const char* key) {
        if (sub.images_.empty()) return;
        static std::atomic<bool> warned{false};
        if (warned.exchange(true)) return;
        std::fprintf(stderr,
            "[xi::Record] WARNING: nesting a sub-Record under '%s' drops its %zu "
            "image(s) — sub-Record images are not carried into the parent "
            "(e.g. a Region mask would vanish). Carry the image at the top level.\n",
            key ? key : "?", sub.images_.size());
    }

    void init_() {
        doc_  = yyjson_mut_doc_new(tls_doc_alc());
        root_ = yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
        box_  = new_box_(doc_);
    }

    // Copy-on-write before a mutation. No-op for the common sole-owned, unfrozen
    // case (one predictable branch). If frozen but we're the SOLE owner (sharers
    // all released, rc==1), just unfreeze — no copy. Otherwise (shared rc>1, or a
    // borrowed view) deep-copy into a fresh sole-owned doc so we never write
    // through a tree someone else still reads.
    void cow_() {
        if (!frozen_) return;
        // Sole LOCAL owner (rc==1) AND not registry-managed ⇒ unfreeze in place.
        // A registry-managed doc must always deep-copy even at local rc==1: a
        // local rc of 1 says nothing about OTHER sides still reading the doc.
        if (box_ && !box_->host_release &&
            box_->rc.load(std::memory_order_acquire) == 1) {
            frozen_ = false;
            return;
        }
        yyjson_mut_doc* nd = yyjson_mut_doc_new(tls_doc_alc());
        yyjson_mut_val* nr = root_ ? yyjson_mut_val_mut_copy(nd, root_)
                                   : yyjson_mut_obj(nd);
        yyjson_mut_doc_set_root(nd, nr);
        release_box_();            // drop our ref on the shared box (borrowed: no-op)
        box_  = new_box_(nd);
        doc_  = nd;
        root_ = nr;
        frozen_ = false;
    }

    // Deep-copy a yyjson object value into a fresh Record (used by getters/Value).
    explicit Record(yyjson_mut_val* obj_val) {
        doc_  = yyjson_mut_doc_new(tls_doc_alc());
        root_ = (obj_val && yyjson_mut_is_obj(obj_val))
                  ? yyjson_mut_val_mut_copy(doc_, obj_val) : yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
        box_  = new_box_(doc_);
    }

    yyjson_mut_val* get_(const char* key) const {
        return root_ ? yyjson_mut_obj_get(root_, key) : nullptr;
    }
    // Set key -> val with replace-if-exists semantics (the key is copied).
    void put_(const char* key, yyjson_mut_val* val) {
        if (!root_ || !val) return;
        yyjson_mut_obj_remove_key(root_, key);
        yyjson_mut_obj_put(root_, yyjson_mut_strcpy(doc_, key), val);
    }
    yyjson_mut_val* ensure_arr_(const char* key) {
        yyjson_mut_val* arr = get_(key);
        if (!arr || !yyjson_mut_is_arr(arr)) {
            arr = yyjson_mut_arr(doc_);
            put_(key, arr);
        }
        return arr;
    }
};

// Guard a plugin's process() against missing inputs in one line. Returns an NA
// Record (naming the first missing field) if any required key is absent, else
// std::nullopt. Usage at the top of a plugin's process:
//
//   if (auto na = xi::require(in, {"current", "baseline"})) return *na;
//
inline std::optional<Record> require(const Record& in,
                                     std::initializer_list<const char*> fields) {
    for (const char* f : fields) {
        if (!in.has(f)) return Record::na(std::string("missing field: ") + f);
        // A field that's PRESENT but holds an NA sub-record ({"$na":...}) must
        // also short-circuit — otherwise has(f) passes and the plugin reads the
        // NA object as a 0-filled record (per-field NA didn't propagate). This is
        // the normal multi-input case: an upstream stage that found nothing
        // returns Typed::na, which lowers to an NA sub-record under its key.
        Record sub = in.get_record(f);   // {} (is_na==false) for a non-object field
        if (sub.is_na()) {
            std::string r = sub.na_reason();
            return Record::na(r.empty() ? (std::string("NA field: ") + f) : r);
        }
    }
    return std::nullopt;
}

} // namespace xi
