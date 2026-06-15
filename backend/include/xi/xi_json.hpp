#pragma once
//
// xi_json.hpp — RAII C++ wrapper around yyjson for plugin internals.
//
// xi::Record is the universal frame-data container (images + JSON). This
// header is for everything else: parsing exchange() commands, building
// reply payloads, reading config files. Same path syntax as Record.
// (Migrated from cJSON 2026-06 — public API unchanged.)
//
// Two roles for the same type:
//   owning  — created by parse/object/array/from; owns a yyjson mutable doc.
//   view    — returned by operator[]/at/for_each; borrows the owner's doc and
//             points at a sub-node. A view must not outlive its owner.
//
// Example:
//
//   xi::Json p = xi::Json::parse(cmd_json);
//   auto cmd = p["command"].as_string();
//   auto reply = xi::Json::object().set("ok", true).set("count", 42);
//   return reply.dump();
//

#include "yyjson.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace xi {

class Json {
public:
    // ---- Construction --------------------------------------------------

    Json() { init_null_(); }
    ~Json() { reset(); }

    Json(const Json& o) {
        if (o.root_) {
            doc_  = yyjson_mut_doc_new(nullptr);
            root_ = yyjson_mut_val_mut_copy(doc_, o.root_);
            yyjson_mut_doc_set_root(doc_, root_);
            owns_ = true;
        }
    }

    Json(Json&& o) noexcept : doc_(o.doc_), root_(o.root_), owns_(o.owns_) {
        o.doc_ = nullptr; o.root_ = nullptr; o.owns_ = false;
    }

    Json& operator=(const Json& o) {
        if (this != &o) {
            reset();
            if (o.root_) {
                doc_  = yyjson_mut_doc_new(nullptr);
                root_ = yyjson_mut_val_mut_copy(doc_, o.root_);
                yyjson_mut_doc_set_root(doc_, root_);
                owns_ = true;
            }
        }
        return *this;
    }

    Json& operator=(Json&& o) noexcept {
        if (this != &o) {
            reset();
            doc_ = o.doc_; root_ = o.root_; owns_ = o.owns_;
            o.doc_ = nullptr; o.root_ = nullptr; o.owns_ = false;
        }
        return *this;
    }

    static Json parse(const char* s) {
        if (!s) return invalid_();
        yyjson_doc* idoc = yyjson_read(s, std::strlen(s), 0);
        if (!idoc) return invalid_();
        yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(idoc, nullptr);
        yyjson_doc_free(idoc);
        if (!mdoc) return invalid_();
        return Json(mdoc, yyjson_mut_doc_get_root(mdoc), true);
    }
    static Json parse(const std::string& s) { return parse(s.c_str()); }

    static Json object() { return rooted_([](yyjson_mut_doc* d){ return yyjson_mut_obj(d); }); }
    static Json array()  { return rooted_([](yyjson_mut_doc* d){ return yyjson_mut_arr(d); }); }
    static Json null()   { return rooted_([](yyjson_mut_doc* d){ return yyjson_mut_null(d); }); }

    static Json from(int v)         { return rooted_([&](yyjson_mut_doc* d){ return yyjson_mut_sint(d, v); }); }
    static Json from(double v)      { return rooted_([&](yyjson_mut_doc* d){ return yyjson_mut_real(d, v); }); }
    static Json from(bool v)        { return rooted_([&](yyjson_mut_doc* d){ return yyjson_mut_bool(d, v); }); }
    static Json from(const char* s) { return rooted_([&](yyjson_mut_doc* d){ return yyjson_mut_strcpy(d, s ? s : ""); }); }
    static Json from(const std::string& s) { return from(s.c_str()); }

    // ---- Type queries --------------------------------------------------

    bool valid()     const { return root_ != nullptr; }
    bool is_null()   const { return !root_ || yyjson_mut_is_null(root_); }
    bool is_bool()   const { return root_ && yyjson_mut_is_bool(root_); }
    bool is_number() const { return root_ && yyjson_mut_is_num(root_); }
    bool is_string() const { return root_ && yyjson_mut_is_str(root_); }
    bool is_array()  const { return root_ && yyjson_mut_is_arr(root_); }
    bool is_object() const { return root_ && yyjson_mut_is_obj(root_); }

    explicit operator bool() const { return valid(); }

    // ---- Value extraction (with defaults) ------------------------------

    int    as_int(int def = 0)       const { return is_number() ? (int)yyjson_mut_get_num(root_) : def; }
    double as_double(double def = 0) const { return is_number() ? yyjson_mut_get_num(root_) : def; }
    bool   as_bool(bool def = false) const { return is_bool() ? yyjson_mut_is_true(root_) : def; }
    std::string as_string(const std::string& def = "") const {
        const char* s = is_string() ? yyjson_mut_get_str(root_) : nullptr;
        return s ? std::string(s) : def;
    }

    // ---- Navigation ----------------------------------------------------

    // Path-aware: ".a.b[3].c", "[0].x", or simple "key".
    Json operator[](const char* key) const {
        bool has_path = false;
        for (const char* p = key; *p; ++p)
            if (*p == '.' || *p == '[') { has_path = true; break; }
        yyjson_mut_val* node = has_path ? resolve_path(root_, key)
            : ((root_ && yyjson_mut_is_obj(root_)) ? yyjson_mut_obj_get(root_, key) : nullptr);
        return Json(doc_, node, false);
    }
    Json operator[](const std::string& key) const { return (*this)[key.c_str()]; }

    Json operator[](int index) const {
        yyjson_mut_val* n = (root_ && yyjson_mut_is_arr(root_)) ? yyjson_mut_arr_get(root_, (size_t)index) : nullptr;
        return Json(doc_, n, false);
    }

    Json at(const char* path) const { return Json(doc_, resolve_path(root_, path), false); }
    Json at(const std::string& p) const { return at(p.c_str()); }

    int size() const {
        if (root_ && yyjson_mut_is_arr(root_)) return (int)yyjson_mut_arr_size(root_);
        if (root_ && yyjson_mut_is_obj(root_)) return (int)yyjson_mut_obj_size(root_);
        return 0;
    }

    // Iterate object: cb(key, value) ; iterate array: cb("", value).
    template <class F>
    void for_each(F&& cb) const {
        if (!root_) return;
        if (yyjson_mut_is_obj(root_)) {
            yyjson_mut_obj_iter it;
            yyjson_mut_obj_iter_init(root_, &it);
            yyjson_mut_val* key;
            while ((key = yyjson_mut_obj_iter_next(&it))) {
                const char* k = yyjson_mut_get_str(key);
                cb(k ? k : "", Json(doc_, yyjson_mut_obj_iter_get_val(key), false));
            }
        } else if (yyjson_mut_is_arr(root_)) {
            yyjson_mut_arr_iter it;
            yyjson_mut_arr_iter_init(root_, &it);
            yyjson_mut_val* val;
            while ((val = yyjson_mut_arr_iter_next(&it))) {
                cb("", Json(doc_, val, false));
            }
        }
    }

    // ---- Builder API (mutating; needs an owning/view with a live doc) ---

    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> &&
                                          !std::is_same_v<T, bool>>>
    Json& set(const char* key, T v)                { return set_node(key, yyjson_mut_sint(doc_, (int64_t)v)); }
    Json& set(const char* key, double v)           { return set_node(key, yyjson_mut_real(doc_, v)); }
    Json& set(const char* key, float v)            { return set_node(key, yyjson_mut_real(doc_, (double)v)); }
    Json& set(const char* key, bool v)             { return set_node(key, yyjson_mut_bool(doc_, v)); }
    Json& set(const char* key, const char* v)      { return set_node(key, yyjson_mut_strcpy(doc_, v ? v : "")); }
    Json& set(const char* key, const std::string& v) { return set(key, v.c_str()); }
    Json& set(const char* key, const Json& nested) {
        return set_node(key, nested.root_ ? yyjson_mut_val_mut_copy(doc_, nested.root_) : yyjson_mut_null(doc_));
    }
    Json& set_null(const char* key)                { return set_node(key, yyjson_mut_null(doc_)); }

    template <typename T,
              typename = std::enable_if_t<std::is_integral_v<T> &&
                                          !std::is_same_v<T, bool>>>
    Json& push(T v)                { return push_node(yyjson_mut_sint(doc_, (int64_t)v)); }
    Json& push(double v)           { return push_node(yyjson_mut_real(doc_, v)); }
    Json& push(float v)            { return push_node(yyjson_mut_real(doc_, (double)v)); }
    Json& push(bool v)             { return push_node(yyjson_mut_bool(doc_, v)); }
    Json& push(const char* v)      { return push_node(yyjson_mut_strcpy(doc_, v ? v : "")); }
    Json& push(const std::string& v) { return push(v.c_str()); }
    Json& push(const Json& nested) {
        return push_node(nested.root_ ? yyjson_mut_val_mut_copy(doc_, nested.root_) : yyjson_mut_null(doc_));
    }

    Json& remove(const char* key) {
        if (root_ && yyjson_mut_is_obj(root_)) yyjson_mut_obj_remove_key(root_, key);
        return *this;
    }

    // ---- Serialization -------------------------------------------------

    std::string dump() const {
        if (!root_) return "null";
        size_t len = 0;
        char* s = yyjson_mut_val_write(root_, 0, &len);
        std::string out = s ? std::string(s, len) : "null";
        std::free(s);
        return out;
    }
    std::string dump_pretty() const {
        if (!root_) return "null";
        size_t len = 0;
        char* s = yyjson_mut_val_write(root_, YYJSON_WRITE_PRETTY, &len);
        std::string out = s ? std::string(s, len) : "null";
        std::free(s);
        return out;
    }

    // ---- Escape hatch --------------------------------------------------

    yyjson_mut_val* raw() const { return root_; }

private:
    yyjson_mut_doc* doc_  = nullptr;
    yyjson_mut_val* root_ = nullptr;
    bool            owns_ = false;

    Json(yyjson_mut_doc* doc, yyjson_mut_val* root, bool owns)
        : doc_(doc), root_(root), owns_(owns) {}

    static Json invalid_() { return Json(nullptr, nullptr, false); }

    template <class F>
    static Json rooted_(F&& make) {
        yyjson_mut_doc* d = yyjson_mut_doc_new(nullptr);
        yyjson_mut_val* r = make(d);
        yyjson_mut_doc_set_root(d, r);
        return Json(d, r, true);
    }

    void init_null_() {
        doc_  = yyjson_mut_doc_new(nullptr);
        root_ = yyjson_mut_null(doc_);
        yyjson_mut_doc_set_root(doc_, root_);
        owns_ = true;
    }

    void reset() {
        if (owns_ && doc_) yyjson_mut_doc_free(doc_);
        doc_ = nullptr; root_ = nullptr; owns_ = false;
    }

    Json& set_node(const char* key, yyjson_mut_val* node) {
        if (!root_ || !yyjson_mut_is_obj(root_) || !node) return *this;
        yyjson_mut_obj_remove_key(root_, key);
        yyjson_mut_obj_put(root_, yyjson_mut_strcpy(doc_, key), node);
        return *this;
    }
    Json& push_node(yyjson_mut_val* node) {
        if (!root_ || !yyjson_mut_is_arr(root_) || !node) return *this;
        yyjson_mut_arr_add_val(root_, node);
        return *this;
    }

    static yyjson_mut_val* resolve_path(yyjson_mut_val* root, const char* p) {
        if (!root || !p) return nullptr;
        yyjson_mut_val* cur = root;
        while (*p && cur) {
            if (*p == '.') ++p;
            if (*p == '[') {
                ++p;
                int idx = 0;
                while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); ++p; }
                if (*p == ']') ++p;
                if (!yyjson_mut_is_arr(cur)) return nullptr;
                cur = yyjson_mut_arr_get(cur, (size_t)idx);
            } else {
                const char* start = p;
                while (*p && *p != '.' && *p != '[') ++p;
                int len = (int)(p - start);
                if (len <= 0) return nullptr;
                std::string key(start, len);
                if (!yyjson_mut_is_obj(cur)) return nullptr;
                cur = yyjson_mut_obj_get(cur, key.c_str());
            }
        }
        return cur;
    }
};

} // namespace xi
