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

#include <map>
#include <memory>
#include <string>

namespace xi {

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

    // --- Accessors ---
    const std::map<std::string, Image>& images() const { return images_; }
    cJSON* json() const { return json_; }

    bool has_image(const std::string& key) const { return images_.count(key) > 0; }
    const Image& get_image(const std::string& key) const { return images_.at(key); }

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
            out += "\"" + k + "\"";
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
