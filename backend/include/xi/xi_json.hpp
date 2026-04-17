#pragma once
//
// xi_json.hpp — RAII C++ wrapper around cJSON for plugin internals.
//
// xi::Record is the universal frame-data container (images + JSON). This
// header is for everything else: parsing exchange() commands, building
// reply payloads, reading config files. Same path syntax as Record.
//
// Two types:
//   xi::Json   — owns a cJSON tree (RAII, parse, build, serialize)
//   xi::Json::View — non-owning view returned by operator[] and friends
//
// Example:
//
//   xi::Json p = xi::Json::parse(cmd_json);
//   auto cmd = p["command"].as_string();
//   if (cmd == "set_threshold") {
//       threshold_ = p["value"].as_int(threshold_);
//   } else if (cmd == "set_roi") {
//       roi_x_ = p["value.x"].as_int(0);
//       roi_y_ = p["value.y"].as_int(0);
//   }
//
//   // Build a reply
//   auto reply = xi::Json::object()
//       .set("ok", true)
//       .set("count", 42)
//       .set("nested", xi::Json::object().set("k", "v"));
//   return reply.dump();
//

#include "cJSON.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace xi {

class Json {
public:
    // ---- Construction --------------------------------------------------

    Json() : root_(cJSON_CreateNull()), owns_(true) {}
    ~Json() { reset(); }

    Json(const Json& o)
        : root_(o.root_ ? cJSON_Duplicate(o.root_, 1) : nullptr), owns_(true) {}

    Json(Json&& o) noexcept : root_(o.root_), owns_(o.owns_) {
        o.root_ = nullptr; o.owns_ = false;
    }

    Json& operator=(const Json& o) {
        if (this != &o) {
            reset();
            root_ = o.root_ ? cJSON_Duplicate(o.root_, 1) : nullptr;
            owns_ = true;
        }
        return *this;
    }

    Json& operator=(Json&& o) noexcept {
        if (this != &o) {
            reset();
            root_ = o.root_; owns_ = o.owns_;
            o.root_ = nullptr; o.owns_ = false;
        }
        return *this;
    }

    static Json parse(const char* s) {
        return Json(s ? cJSON_Parse(s) : nullptr, true);
    }
    static Json parse(const std::string& s) { return parse(s.c_str()); }

    static Json object() { return Json(cJSON_CreateObject(), true); }
    static Json array()  { return Json(cJSON_CreateArray(),  true); }
    static Json null()   { return Json(cJSON_CreateNull(),   true); }

    static Json from(int v)         { return Json(cJSON_CreateNumber(v),    true); }
    static Json from(double v)      { return Json(cJSON_CreateNumber(v),    true); }
    static Json from(bool v)        { return Json(cJSON_CreateBool(v),      true); }
    static Json from(const char* s) { return Json(cJSON_CreateString(s),    true); }
    static Json from(const std::string& s) { return from(s.c_str()); }

    // ---- Type queries --------------------------------------------------

    bool valid()     const { return root_ != nullptr; }
    bool is_null()   const { return !root_ || cJSON_IsNull(root_); }
    bool is_bool()   const { return root_ && cJSON_IsBool(root_); }
    bool is_number() const { return root_ && cJSON_IsNumber(root_); }
    bool is_string() const { return root_ && cJSON_IsString(root_); }
    bool is_array()  const { return root_ && cJSON_IsArray(root_); }
    bool is_object() const { return root_ && cJSON_IsObject(root_); }

    explicit operator bool() const { return valid(); }

    // ---- Value extraction (with defaults) ------------------------------

    int as_int(int def = 0) const {
        return is_number() ? (int)root_->valuedouble : def;
    }
    double as_double(double def = 0) const {
        return is_number() ? root_->valuedouble : def;
    }
    bool as_bool(bool def = false) const {
        return is_bool() ? cJSON_IsTrue(root_) : def;
    }
    std::string as_string(const std::string& def = "") const {
        return is_string() ? std::string(root_->valuestring) : def;
    }

    // ---- Navigation ----------------------------------------------------

    // Path-aware: ".a.b[3].c", "[0].x", or simple "key".
    Json operator[](const char* key) const {
        bool has_path = false;
        for (const char* p = key; *p; ++p)
            if (*p == '.' || *p == '[') { has_path = true; break; }
        cJSON* node = has_path ? resolve_path(root_, key)
                               : (root_ ? cJSON_GetObjectItem(root_, key) : nullptr);
        return Json(node, false);
    }
    Json operator[](const std::string& key) const { return (*this)[key.c_str()]; }

    Json operator[](int index) const {
        return Json(cJSON_IsArray(root_) ? cJSON_GetArrayItem(root_, index) : nullptr, false);
    }

    Json at(const char* path) const { return Json(resolve_path(root_, path), false); }
    Json at(const std::string& p) const { return at(p.c_str()); }

    int size() const {
        if (cJSON_IsArray(root_) || cJSON_IsObject(root_)) return cJSON_GetArraySize(root_);
        return 0;
    }

    // Iterate object: cb(key, value) ; iterate array: cb(index_str, value).
    template <class F>
    void for_each(F&& cb) const {
        if (!root_) return;
        cJSON* it = root_->child;
        for (int i = 0; it; it = it->next, ++i) {
            const char* key = it->string ? it->string : "";
            cb(key, Json(it, false));
        }
    }

    // ---- Builder API (mutating, only valid on owning root) -------------

    Json& set(const char* key, int v)              { return set_node(key, cJSON_CreateNumber(v)); }
    Json& set(const char* key, double v)           { return set_node(key, cJSON_CreateNumber(v)); }
    Json& set(const char* key, bool v)             { return set_node(key, cJSON_CreateBool(v)); }
    Json& set(const char* key, const char* v)      { return set_node(key, cJSON_CreateString(v ? v : "")); }
    Json& set(const char* key, const std::string& v) { return set(key, v.c_str()); }
    Json& set(const char* key, const Json& nested) {
        return set_node(key, nested.root_ ? cJSON_Duplicate(nested.root_, 1) : cJSON_CreateNull());
    }
    Json& set_null(const char* key)                { return set_node(key, cJSON_CreateNull()); }

    Json& push(int v)              { return push_node(cJSON_CreateNumber(v)); }
    Json& push(double v)           { return push_node(cJSON_CreateNumber(v)); }
    Json& push(bool v)             { return push_node(cJSON_CreateBool(v)); }
    Json& push(const char* v)      { return push_node(cJSON_CreateString(v ? v : "")); }
    Json& push(const std::string& v) { return push(v.c_str()); }
    Json& push(const Json& nested) {
        return push_node(nested.root_ ? cJSON_Duplicate(nested.root_, 1) : cJSON_CreateNull());
    }

    Json& remove(const char* key) {
        if (cJSON_IsObject(root_)) cJSON_DeleteItemFromObject(root_, key);
        return *this;
    }

    // ---- Serialization -------------------------------------------------

    std::string dump() const {
        if (!root_) return "null";
        char* s = cJSON_PrintUnformatted(root_);
        std::string out = s ? s : "null";
        std::free(s);
        return out;
    }
    std::string dump_pretty() const {
        if (!root_) return "null";
        char* s = cJSON_Print(root_);
        std::string out = s ? s : "null";
        std::free(s);
        return out;
    }

    // ---- Escape hatch --------------------------------------------------

    cJSON* raw() const { return root_; }

private:
    cJSON* root_ = nullptr;
    bool   owns_ = false;

    Json(cJSON* n, bool owns) : root_(n), owns_(owns) {}

    void reset() {
        if (owns_ && root_) cJSON_Delete(root_);
        root_ = nullptr;
        owns_ = false;
    }

    Json& set_node(const char* key, cJSON* node) {
        if (!root_ || !cJSON_IsObject(root_) || !node) { if (node) cJSON_Delete(node); return *this; }
        cJSON_DeleteItemFromObject(root_, key);
        cJSON_AddItemToObject(root_, key, node);
        return *this;
    }
    Json& push_node(cJSON* node) {
        if (!root_ || !cJSON_IsArray(root_) || !node) { if (node) cJSON_Delete(node); return *this; }
        cJSON_AddItemToArray(root_, node);
        return *this;
    }

    static cJSON* resolve_path(cJSON* root, const char* p) {
        if (!root || !p) return nullptr;
        cJSON* cur = root;
        while (*p && cur) {
            if (*p == '.') ++p;
            if (*p == '[') {
                ++p;
                int idx = 0;
                while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); ++p; }
                if (*p == ']') ++p;
                if (!cJSON_IsArray(cur)) return nullptr;
                cur = cJSON_GetArrayItem(cur, idx);
            } else {
                const char* start = p;
                while (*p && *p != '.' && *p != '[') ++p;
                int len = (int)(p - start);
                if (len <= 0) return nullptr;
                char key[256];
                if (len >= 256) len = 255;
                std::memcpy(key, start, len);
                key[len] = 0;
                if (!cJSON_IsObject(cur)) return nullptr;
                cur = cJSON_GetObjectItem(cur, key);
            }
        }
        return cur;
    }
};

} // namespace xi
