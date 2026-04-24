#pragma once
//
// xi_record.hpp — unified output type for inspection steps.
//
// A Record bundles named images + schemaless JSON data. Built on cJSON
// so all escaping, nesting, and serialization is handled correctly.
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

#include "xi_image.hpp"
#include "cJSON.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>

namespace xi {

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

class Record {
public:
    Record() : json_(cJSON_CreateObject()) {}

    ~Record() {
        if (json_) cJSON_Delete(json_);
    }

    Record(const Record& o) : images_(o.images_) {
        json_ = o.json_ ? cJSON_Duplicate(o.json_, true) : cJSON_CreateObject();
    }

    Record& operator=(const Record& o) {
        if (this != &o) {
            images_ = o.images_;
            if (json_) cJSON_Delete(json_);
            json_ = o.json_ ? cJSON_Duplicate(o.json_, true) : cJSON_CreateObject();
        }
        return *this;
    }

    Record(Record&& o) noexcept : images_(std::move(o.images_)), json_(o.json_) {
        o.json_ = nullptr;
    }

    Record& operator=(Record&& o) noexcept {
        if (this != &o) {
            images_ = std::move(o.images_);
            if (json_) cJSON_Delete(json_);
            json_ = o.json_;
            o.json_ = nullptr;
        }
        return *this;
    }

    // --- Image builder ---
    Record& image(const std::string& key, Image img) {
        images_[key] = std::move(img);
        return *this;
    }

    // --- Data builders (add to the JSON object) ---
    Record& set(const std::string& key, int v) {
        cJSON_DeleteItemFromObject(json_, key.c_str());
        cJSON_AddNumberToObject(json_, key.c_str(), v);
        return *this;
    }
    Record& set(const std::string& key, double v) {
        cJSON_DeleteItemFromObject(json_, key.c_str());
        cJSON_AddNumberToObject(json_, key.c_str(), v);
        return *this;
    }
    Record& set(const std::string& key, bool v) {
        cJSON_DeleteItemFromObject(json_, key.c_str());
        cJSON_AddBoolToObject(json_, key.c_str(), v ? 1 : 0);
        return *this;
    }
    Record& set(const std::string& key, const std::string& v) {
        cJSON_DeleteItemFromObject(json_, key.c_str());
        cJSON_AddStringToObject(json_, key.c_str(), v.c_str());
        return *this;
    }
    Record& set(const std::string& key, const char* v) {
        return set(key, std::string(v));
    }

    // Nest a sub-Record as a JSON object
    Record& set(const std::string& key, const Record& sub) {
        cJSON_DeleteItemFromObject(json_, key.c_str());
        cJSON* dup = sub.json_ ? cJSON_Duplicate(sub.json_, true) : cJSON_CreateObject();
        cJSON_AddItemToObject(json_, key.c_str(), dup);
        return *this;
    }

    // Add raw cJSON item (for advanced use)
    Record& set_raw(const std::string& key, cJSON* item) {
        cJSON_DeleteItemFromObject(json_, key.c_str());
        cJSON_AddItemToObject(json_, key.c_str(), item);
        return *this;
    }

    // --- Array builders ---
    Record& push(const std::string& key, int v) {
        ensure_array(key);
        cJSON_AddItemToArray(cJSON_GetObjectItem(json_, key.c_str()), cJSON_CreateNumber(v));
        return *this;
    }
    Record& push(const std::string& key, double v) {
        ensure_array(key);
        cJSON_AddItemToArray(cJSON_GetObjectItem(json_, key.c_str()), cJSON_CreateNumber(v));
        return *this;
    }
    Record& push(const std::string& key, bool v) {
        ensure_array(key);
        cJSON_AddItemToArray(cJSON_GetObjectItem(json_, key.c_str()), cJSON_CreateBool(v));
        return *this;
    }
    Record& push(const std::string& key, const std::string& v) {
        ensure_array(key);
        cJSON_AddItemToArray(cJSON_GetObjectItem(json_, key.c_str()), cJSON_CreateString(v.c_str()));
        return *this;
    }
    Record& push(const std::string& key, const Record& sub) {
        ensure_array(key);
        cJSON* dup = sub.json_ ? cJSON_Duplicate(sub.json_, true) : cJSON_CreateObject();
        cJSON_AddItemToArray(cJSON_GetObjectItem(json_, key.c_str()), dup);
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
        explicit Value(cJSON* node) : node_(node) {}

        // Chain into object key
        Value operator[](const char* key) const {
            if (!node_ || !cJSON_IsObject(node_)) return {};
            return Value(cJSON_GetObjectItem(node_, key));
        }
        Value operator[](const std::string& key) const { return (*this)[key.c_str()]; }

        // Chain into array index
        Value operator[](int index) const {
            if (!node_ || !cJSON_IsArray(node_)) return {};
            return Value(cJSON_GetArrayItem(node_, index));
        }

        // Terminal reads with defaults
        int         as_int(int def = 0)                       const { return (node_ && cJSON_IsNumber(node_)) ? node_->valueint : def; }
        double      as_double(double def = 0.0)               const { return (node_ && cJSON_IsNumber(node_)) ? node_->valuedouble : def; }
        bool        as_bool(bool def = false)                  const { return node_ ? cJSON_IsTrue(node_) : def; }
        std::string as_string(const std::string& def = "")    const { return (node_ && cJSON_IsString(node_)) ? node_->valuestring : def; }

        // Array length
        int size() const { return (node_ && cJSON_IsArray(node_)) ? cJSON_GetArraySize(node_) : 0; }

        // Check existence
        bool exists()    const { return node_ != nullptr; }
        bool is_null()   const { return !node_ || cJSON_IsNull(node_); }
        bool is_object() const { return node_ && cJSON_IsObject(node_); }
        bool is_array()  const { return node_ && cJSON_IsArray(node_); }
        bool is_number() const { return node_ && cJSON_IsNumber(node_); }
        bool is_string() const { return node_ && cJSON_IsString(node_); }
        bool is_bool()   const { return node_ && cJSON_IsBool(node_); }

        // Extract as a standalone Record (deep copy)
        Record as_record() const {
            if (!node_ || !cJSON_IsObject(node_)) return {};
            Record r;
            cJSON_Delete(r.json_);
            r.json_ = cJSON_Duplicate(node_, true);
            return r;
        }

        // Iterate array: for (int i = 0; i < val.size(); ++i) val[i]...
        // Or use the raw cJSON pointer for cJSON_ArrayForEach
        cJSON* raw() const { return node_; }

        // Path expression access:
        //   val.at(".a.b[3].c")     object→object→array[3]→object
        //   val.at("[1].x")         array[1]→object
        //   val.at("a.b.c")         same as .a.b.c
        Value at(const char* path) const {
            return Value(resolve_path(node_, path));
        }
        Value at(const std::string& path) const { return at(path.c_str()); }

    private:
        cJSON* node_;

        static cJSON* resolve_path(cJSON* root, const char* p) {
            if (!root || !p) return nullptr;
            cJSON* cur = root;
            while (*p && cur) {
                if (*p == '.') ++p;  // skip leading or separator dot
                if (*p == '[') {
                    // Array index: [N]
                    ++p;
                    int idx = 0;
                    while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); ++p; }
                    if (*p == ']') ++p;
                    if (!cJSON_IsArray(cur)) return nullptr;
                    cur = cJSON_GetArrayItem(cur, idx);
                } else {
                    // Object key: read until next . or [ or end
                    const char* start = p;
                    while (*p && *p != '.' && *p != '[') ++p;
                    if (p == start) return nullptr;
                    // Extract key
                    char key[256];
                    int len = (int)(p - start);
                    if (len >= 256) len = 255;
                    memcpy(key, start, len);
                    key[len] = 0;
                    if (!cJSON_IsObject(cur)) return nullptr;
                    cur = cJSON_GetObjectItem(cur, key);
                }
            }
            return cur;
        }
    };

    // Entry point for chained access
    Value operator[](const char* key) const {
        // If key contains '.' or '[', treat as path expression
        bool is_path = false;
        for (const char* p = key; *p; ++p) {
            if (*p == '.' || *p == '[') { is_path = true; break; }
        }
        if (is_path) return Value(json_).at(key);
        return Value(cJSON_GetObjectItem(json_, key));
    }
    Value operator[](const std::string& key) const {
        return (*this)[key.c_str()];
    }
    // Explicit path access (always uses path parser)
    Value at(const char* path) const { return Value(json_).at(path); }
    Value at(const std::string& path) const { return at(path.c_str()); }

    // --- Data getters (with defaults) ---

    int get_int(const std::string& key, int def = 0) const {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        return (item && cJSON_IsNumber(item)) ? item->valueint : def;
    }

    double get_double(const std::string& key, double def = 0.0) const {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        return (item && cJSON_IsNumber(item)) ? item->valuedouble : def;
    }

    bool get_bool(const std::string& key, bool def = false) const {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        return item ? cJSON_IsTrue(item) : def;
    }

    std::string get_string(const std::string& key, const std::string& def = "") const {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        return (item && cJSON_IsString(item)) ? item->valuestring : def;
    }

    bool has(const std::string& key) const {
        return cJSON_GetObjectItem(json_, key.c_str()) != nullptr;
    }

    // Get a nested Record (returns empty Record if not found)
    Record get_record(const std::string& key) const {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        if (!item || !cJSON_IsObject(item)) return {};
        Record r;
        cJSON_Delete(r.json_);
        r.json_ = cJSON_Duplicate(item, true);
        return r;
    }

    // Get array length (returns 0 if key not found or not array)
    int get_array_size(const std::string& key) const {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        return (item && cJSON_IsArray(item)) ? cJSON_GetArraySize(item) : 0;
    }

    // Get array element as Record (for arrays of objects)
    Record get_array_item(const std::string& key, int index) const {
        cJSON* arr = cJSON_GetObjectItem(json_, key.c_str());
        if (!arr || !cJSON_IsArray(arr)) return {};
        cJSON* item = cJSON_GetArrayItem(arr, index);
        if (!item || !cJSON_IsObject(item)) return {};
        Record r;
        cJSON_Delete(r.json_);
        r.json_ = cJSON_Duplicate(item, true);
        return r;
    }

    // --- Image accessors ---
    const std::map<std::string, Image>& images() const { return images_; }
    cJSON* json() const { return json_; }

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
        if (!json_) return "{}";
        char* s = cJSON_PrintUnformatted(json_);
        std::string out(s);
        cJSON_free(s);
        return out;
    }

    std::string data_json_pretty() const {
        if (!json_) return "{}";
        char* s = cJSON_Print(json_);
        std::string out(s);
        cJSON_free(s);
        return out;
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

    bool empty() const { return images_.empty() && (!json_ || !json_->child); }

private:
    std::map<std::string, Image> images_;
    cJSON* json_;

    void ensure_array(const std::string& key) {
        cJSON* item = cJSON_GetObjectItem(json_, key.c_str());
        if (!item || !cJSON_IsArray(item)) {
            cJSON_DeleteItemFromObject(json_, key.c_str());
            cJSON_AddItemToObject(json_, key.c_str(), cJSON_CreateArray());
        }
    }
};

} // namespace xi
