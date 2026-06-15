#pragma once
//
// xi_record.hpp — unified output type for inspection steps.
//
// A Record bundles named images + schemaless JSON data. Backed by yyjson
// (a mutable doc) so building is incremental + mutable and reading/serializing
// are fast; escaping, nesting, and serialization are handled correctly.
// (Migrated from cJSON 2026-06 — see docs/design/wire-format-msgpack.md.)
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

// Non-finite doubles can't be represented in JSON — they serialise as the
// literal `null`, which then reads back as the caller's default (typically 0.0),
// silently turning an invalid measurement into a plausible zero. We store them
// as explicit string sentinels instead; these helpers convert both ways so a
// non-finite value survives the Record↔JSON round-trip and stays visible.
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
public:
    Record() { init_(); }

    ~Record() { if (owns_doc_ && doc_) yyjson_mut_doc_free(doc_); }

    Record(const Record& o) : images_(o.images_) {
        // A copy is always an independent, owned, mutable doc — even when `o` is
        // a borrowed read-only view (reading o.root_ is safe; we allocate ours).
        doc_  = yyjson_mut_doc_new(tls_doc_alc());
        root_ = o.root_ ? yyjson_mut_val_mut_copy(doc_, o.root_) : yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
    }

    Record& operator=(const Record& o) {
        if (this != &o) {
            images_ = o.images_;
            if (owns_doc_ && doc_) yyjson_mut_doc_free(doc_);
            doc_  = yyjson_mut_doc_new(tls_doc_alc());
            root_ = o.root_ ? yyjson_mut_val_mut_copy(doc_, o.root_) : yyjson_mut_obj(doc_);
            yyjson_mut_doc_set_root(doc_, root_);
            owns_doc_ = true; frozen_ = false;
        }
        return *this;
    }

    Record(Record&& o) noexcept
        : images_(std::move(o.images_)), doc_(o.doc_), root_(o.root_),
          owns_doc_(o.owns_doc_), frozen_(o.frozen_) {
        o.doc_ = nullptr; o.root_ = nullptr; o.owns_doc_ = false;
    }

    Record& operator=(Record&& o) noexcept {
        if (this != &o) {
            images_ = std::move(o.images_);
            if (owns_doc_ && doc_) yyjson_mut_doc_free(doc_);
            doc_ = o.doc_; root_ = o.root_;
            owns_doc_ = o.owns_doc_; frozen_ = o.frozen_;
            o.doc_ = nullptr; o.root_ = nullptr; o.owns_doc_ = false;
        }
        return *this;
    }

    // --- γ: borrowed read-only view over a doc owned elsewhere ---------------
    //
    // Wrap a yyjson_mut_doc the host handed us across the ABI (in-process zero-
    // serialize input). The view does NOT own the doc — its dtor frees nothing —
    // and is FROZEN: the first mutation copy-on-writes into a fresh owned doc
    // (host-pool-allocated), leaving the borrowed doc untouched. Reads hit the
    // borrowed nodes directly (no parse, no copy).
    static Record from_doc_view(yyjson_mut_doc* borrowed) {
        Record r;
        if (borrowed) {
            if (r.owns_doc_ && r.doc_) yyjson_mut_doc_free(r.doc_);
            r.doc_  = borrowed;
            r.root_ = yyjson_mut_doc_get_root(borrowed);
            r.owns_doc_ = false;
            r.frozen_   = true;
        }
        return r;
    }

    // Release ownership of the underlying doc to the caller (ABI output handoff).
    // After this the Record no longer owns/frees the doc; the caller adopts it.
    yyjson_mut_doc* release_doc() {
        yyjson_mut_doc* d = doc_;
        doc_ = nullptr; root_ = nullptr; owns_doc_ = false;
        return d;
    }

    // Adopt ownership of a doc handed across the ABI (caller side of the output
    // handoff). The doc was built with the host allocator, so freeing it here
    // (via doc->alc) is cross-DLL-safe.
    static Record adopt_doc(yyjson_mut_doc* owned) {
        Record r;
        if (owned) {
            if (r.owns_doc_ && r.doc_) yyjson_mut_doc_free(r.doc_);
            r.doc_  = owned;
            r.root_ = yyjson_mut_doc_get_root(owned);
            r.owns_doc_ = true;
            r.frozen_   = false;
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
    Record& set(const std::string& key, const Record& sub) {
        cow_();
        yyjson_mut_val* dup = sub.root_ ? yyjson_mut_val_mut_copy(doc_, sub.root_)
                                        : yyjson_mut_obj(doc_);
        put_(key.c_str(), dup);
        return *this;
    }

    // Add a raw yyjson value that ALREADY belongs to this Record's doc (advanced).
    // NOTE: not valid on a frozen/borrowed view — `item` must come from THIS
    // Record's (owned) doc, so callers building `item` must already have an owned
    // doc; cow_() here would orphan an item built against the pre-COW doc.
    Record& set_raw(const std::string& key, yyjson_mut_val* item) {
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
        yyjson_mut_arr_add_val(ensure_arr_(key.c_str()), yyjson_mut_real(doc_, v));
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
            return is_number() ? (int)yyjson_mut_get_num(node_) : def;
        }
        double as_double(double def = 0.0) const {
            if (is_number()) return yyjson_mut_get_num(node_);
            if (node_ && yyjson_mut_is_str(node_)) { double v; if (nonfinite_from_str(yyjson_mut_get_str(node_), v)) return v; }
            return def;
        }
        bool as_bool(bool def = false) const { return node_ ? yyjson_mut_is_true(node_) : def; }
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
    int get_int(const std::string& key, int def = 0) const {
        yyjson_mut_val* it = get_(key.c_str());
        return (it && yyjson_mut_is_num(it)) ? (int)yyjson_mut_get_num(it) : def;
    }
    double get_double(const std::string& key, double def = 0.0) const {
        yyjson_mut_val* it = get_(key.c_str());
        if (it && yyjson_mut_is_num(it)) return yyjson_mut_get_num(it);
        if (it && yyjson_mut_is_str(it)) { double v; if (nonfinite_from_str(yyjson_mut_get_str(it), v)) return v; }
        return def;
    }
    bool get_bool(const std::string& key, bool def = false) const {
        yyjson_mut_val* it = get_(key.c_str());
        return it ? yyjson_mut_is_true(it) : def;
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
            yyjson_mut_doc_free(r.doc_);
            r.doc_  = yyjson_mut_doc_new(tls_doc_alc());
            r.root_ = yyjson_val_mut_copy(r.doc_, iroot);
            yyjson_mut_doc_set_root(r.doc_, r.root_);
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
    // γ ownership flags. owns_doc_: this Record frees doc_ on destruction
    // (false for a borrowed view / after release_doc). frozen_: doc_ is shared/
    // borrowed and read-only — the first mutation copy-on-writes (cow_) into a
    // fresh owned doc. Both default to "owned, writable" so every existing
    // construction path behaves exactly as before.
    bool owns_doc_ = true;
    bool frozen_   = false;

    void init_() {
        doc_  = yyjson_mut_doc_new(tls_doc_alc());
        root_ = yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
    }

    // Copy-on-write: if the doc is frozen (a borrowed view, or—later—a shared
    // refcounted doc), deep-copy it into a fresh OWNED doc before mutating, so
    // we never write through someone else's tree. No-op for the common owned,
    // unfrozen case (one predictable branch on the build path).
    void cow_() {
        if (!frozen_) return;
        yyjson_mut_doc* nd = yyjson_mut_doc_new(tls_doc_alc());
        yyjson_mut_val* nr = root_ ? yyjson_mut_val_mut_copy(nd, root_)
                                   : yyjson_mut_obj(nd);
        yyjson_mut_doc_set_root(nd, nr);
        // Do NOT free doc_: it is borrowed/shared (owns_doc_ == false here, or a
        // future shared owner still holds it).
        doc_  = nd;
        root_ = nr;
        owns_doc_ = true;
        frozen_   = false;
    }

    // Deep-copy a yyjson object value into a fresh Record (used by getters/Value).
    explicit Record(yyjson_mut_val* obj_val) {
        doc_  = yyjson_mut_doc_new(tls_doc_alc());
        root_ = (obj_val && yyjson_mut_is_obj(obj_val))
                  ? yyjson_mut_val_mut_copy(doc_, obj_val) : yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
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
    for (const char* f : fields)
        if (!in.has(f)) return Record::na(std::string("missing field: ") + f);
    return std::nullopt;
}

} // namespace xi
