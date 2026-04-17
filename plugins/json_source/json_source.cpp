//
// json_source.cpp — emits a configurable JSON record.
//
// The user edits the JSON via the plugin's GUI; the stored object is
// produced as the output Record on every process() call. Useful for
// injecting test fixtures, configuration, or manual data into a pipeline.
//
// Exchange commands:
//   { "command": "set_data", "value": <json object> }
//   { "command": "get_status" }
//

#include <xi/xi_abi.hpp>

#include <cstring>
#include <string>

namespace {

// Set a value at a JSON path, creating intermediate objects/arrays as needed.
//   ".a.b[2].c"  → object→object→array[idx 2]→object
//   "[1].x"      → array[idx 1]→object
//   "a.b.c"      → same as ".a.b.c"
// `value` is duplicated; caller still owns it.
bool json_set_path(cJSON* root, const char* path, const cJSON* value) {
    if (!root || !path || !value) return false;
    cJSON* cur = path[0] && (path[0] != '.' && path[0] != '[') ? root : root;
    const char* p = path;

    auto skip_dot = [](const char*& s) { if (*s == '.') ++s; };

    while (*p) {
        skip_dot(p);
        if (!*p) break;

        bool is_index = (*p == '[');
        char key[256] = {0};
        int idx = 0;
        const char* seg_end;

        if (is_index) {
            ++p;
            while (*p >= '0' && *p <= '9') { idx = idx * 10 + (*p - '0'); ++p; }
            if (*p == ']') ++p;
            seg_end = p;
        } else {
            const char* start = p;
            while (*p && *p != '.' && *p != '[') ++p;
            int len = (int)(p - start);
            if (len <= 0 || len >= 256) return false;
            std::memcpy(key, start, len);
            key[len] = 0;
            seg_end = p;
        }

        // Terminal if nothing meaningful follows (skip optional dot).
        const char* peek = seg_end;
        while (*peek == '.') ++peek;
        bool is_terminal = (*peek == 0);

        if (is_terminal) {
            cJSON* dup = cJSON_Duplicate(value, 1);
            if (!dup) return false;
            if (is_index) {
                if (!cJSON_IsArray(cur)) { cJSON_Delete(dup); return false; }
                int sz = cJSON_GetArraySize(cur);
                while (sz < idx) { cJSON_AddItemToArray(cur, cJSON_CreateNull()); ++sz; }
                if (sz == idx) cJSON_AddItemToArray(cur, dup);
                else           cJSON_ReplaceItemInArray(cur, idx, dup);
            } else {
                if (!cJSON_IsObject(cur)) { cJSON_Delete(dup); return false; }
                cJSON_DeleteItemFromObject(cur, key);
                cJSON_AddItemToObject(cur, key, dup);
            }
            return true;
        }

        // Non-terminal: descend, creating the right container kind for the
        // next segment.
        const char* next = seg_end;
        while (*next == '.') ++next;
        bool next_is_index = (*next == '[');

        cJSON* child = nullptr;
        if (is_index) {
            if (!cJSON_IsArray(cur)) return false;
            int sz = cJSON_GetArraySize(cur);
            while (sz <= idx) {
                cJSON_AddItemToArray(cur, next_is_index ? cJSON_CreateArray() : cJSON_CreateObject());
                ++sz;
            }
            child = cJSON_GetArrayItem(cur, idx);
        } else {
            if (!cJSON_IsObject(cur)) return false;
            child = cJSON_GetObjectItem(cur, key);
            if (!child) {
                child = next_is_index ? cJSON_CreateArray() : cJSON_CreateObject();
                cJSON_AddItemToObject(cur, key, child);
            }
        }
        cur = child;
    }
    return true;
}

// Apply one {key, value} patch to dst. Returns true if applied.
bool apply_patch(cJSON* dst, cJSON* patch) {
    cJSON* k = cJSON_GetObjectItem(patch, "key");
    cJSON* v = cJSON_GetObjectItem(patch, "value");
    if (!k || !cJSON_IsString(k) || !v) return false;
    return json_set_path(dst, k->valuestring, v);
}

} // namespace

class JsonSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Input may carry runtime patches that mutate the GUI-edited JSON before
    // it's emitted. Two accepted shapes (both work; either is fine):
    //   single patch:  { "key": ".a.b[2]", "value": <anything> }
    //   batch:         { "patches": [ { "key": ".x", "value": 1 }, ... ] }
    // The stored JSON is not modified — only the emitted Record is.
    xi::Record process(const xi::Record& input) override {
        cJSON* base = cJSON_Parse(stored_json_.c_str());
        if (!base) base = cJSON_CreateObject();

        // Pull patches from input (re-parse via JSON since Record doesn't
        // expose its cJSON* directly).
        std::string in_json = input.data_json();
        cJSON* in = cJSON_Parse(in_json.c_str());
        if (in) {
            cJSON* batch = cJSON_GetObjectItem(in, "patches");
            if (batch && cJSON_IsArray(batch)) {
                cJSON* it = batch->child;
                while (it) { apply_patch(base, it); it = it->next; }
            } else if (cJSON_GetObjectItem(in, "key")) {
                apply_patch(base, in);
            }
            cJSON_Delete(in);
        }

        xi::Record result;
        cJSON* item = base->child;
        while (item) {
            if (cJSON_IsNumber(item))      result.set(item->string, item->valuedouble);
            else if (cJSON_IsBool(item))   result.set(item->string, cJSON_IsTrue(item) ? true : false);
            else if (cJSON_IsString(item)) result.set(item->string, std::string(item->valuestring));
            else                           result.set_raw(item->string, cJSON_Duplicate(item, true));
            item = item->next;
        }
        cJSON_Delete(base);
        return result;
    }

    std::string exchange(const std::string& cmd) override {
        cJSON* p = cJSON_Parse(cmd.c_str());
        if (!p) return get_def();
        cJSON* c = cJSON_GetObjectItem(p, "command");
        cJSON* v = cJSON_GetObjectItem(p, "value");
        if (c && cJSON_IsString(c)) {
            std::string command = c->valuestring;
            if (command == "set_data" && v) {
                char* s = cJSON_PrintUnformatted(v);
                if (s) { stored_json_.assign(s); std::free(s); }
            } else if (command == "reset") {
                stored_json_ = "{}";
            }
            // get_status falls through to get_def() below
        }
        cJSON_Delete(p);
        return get_def();
    }

    std::string get_def() const override {
        // get_def returns wrapper { data: <stored> } so the UI knows the
        // value is the user JSON, not part of the def itself.
        cJSON* root = cJSON_CreateObject();
        cJSON* data = cJSON_Parse(stored_json_.c_str());
        if (!data) data = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "data", data);
        char* s = cJSON_PrintUnformatted(root);
        std::string out = s ? s : "{\"data\":{}}";
        std::free(s);
        cJSON_Delete(root);
        return out;
    }

    bool set_def(const std::string& json) override {
        cJSON* root = cJSON_Parse(json.c_str());
        if (!root) return false;
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (data) {
            char* s = cJSON_PrintUnformatted(data);
            if (s) { stored_json_.assign(s); std::free(s); }
        }
        cJSON_Delete(root);
        return true;
    }

private:
    std::string stored_json_ = "{}";
};

XI_PLUGIN_IMPL(JsonSource)
