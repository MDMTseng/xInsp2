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
#include <xi/xi_contract.hpp>   // fail-loud required command payload + schema skew
#include "yyjson.h"   // this plugin does raw yyjson path-building internally

#include "json_source_keys.gen.h"   // guard 1: command/config/patch key names, once

#include <cstdlib>
#include <cstring>
#include <string>

// Guard 1: the plugin's own readers compile from the SAME constants the typed
// view (json_source_io.h) builds from, so a key rename can't drift.
namespace jkeys = xi::json_source::keys;

namespace {

// Set a value at a JSON path, creating intermediate objects/arrays as needed.
//   ".a.b[2].c"  → object→object→array[idx 2]→object
//   "[1].x"      → array[idx 1]→object
//   "a.b.c"      → same as ".a.b.c"
// `value` is deep-copied into `doc`; caller still owns it.
bool json_set_path(yyjson_mut_doc* doc, yyjson_mut_val* root,
                   const char* path, yyjson_mut_val* value) {
    if (!doc || !root || !path || !value) return false;
    yyjson_mut_val* cur = root;
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
            yyjson_mut_val* dup = yyjson_mut_val_mut_copy(doc, value);
            if (!dup) return false;
            if (is_index) {
                if (!yyjson_mut_is_arr(cur)) return false;
                int sz = (int)yyjson_mut_arr_size(cur);
                while (sz < idx) { yyjson_mut_arr_append(cur, yyjson_mut_null(doc)); ++sz; }
                if (sz == idx) yyjson_mut_arr_append(cur, dup);
                else           yyjson_mut_arr_replace(cur, (size_t)idx, dup);
            } else {
                if (!yyjson_mut_is_obj(cur)) return false;
                yyjson_mut_obj_put(cur, yyjson_mut_strcpy(doc, key), dup);
            }
            return true;
        }

        // Non-terminal: descend, creating the right container kind for the
        // next segment.
        const char* next = seg_end;
        while (*next == '.') ++next;
        bool next_is_index = (*next == '[');

        yyjson_mut_val* child = nullptr;
        if (is_index) {
            if (!yyjson_mut_is_arr(cur)) return false;
            int sz = (int)yyjson_mut_arr_size(cur);
            while (sz <= idx) {
                yyjson_mut_arr_append(cur, next_is_index ? yyjson_mut_arr(doc) : yyjson_mut_obj(doc));
                ++sz;
            }
            child = yyjson_mut_arr_get(cur, (size_t)idx);
        } else {
            if (!yyjson_mut_is_obj(cur)) return false;
            child = yyjson_mut_obj_get(cur, key);
            if (!child) {
                child = next_is_index ? yyjson_mut_arr(doc) : yyjson_mut_obj(doc);
                yyjson_mut_obj_add_val(doc, cur, key, child);
            }
        }
        cur = child;
    }
    return true;
}

// Apply one {key, value} patch to dst. Returns true if applied.
bool apply_patch(yyjson_mut_doc* doc, yyjson_mut_val* dst, yyjson_mut_val* patch) {
    yyjson_mut_val* k = yyjson_mut_obj_get(patch, jkeys::kKey);
    yyjson_mut_val* v = yyjson_mut_obj_get(patch, jkeys::kValue);
    if (!k || !yyjson_mut_is_str(k) || !v) return false;
    return json_set_path(doc, dst, yyjson_mut_get_str(k), v);
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
        // Parse the stored JSON into a mutable doc we can patch.
        yyjson_doc* base_rd = yyjson_read(stored_json_.c_str(), stored_json_.size(), 0);
        yyjson_mut_doc* doc = base_rd ? yyjson_doc_mut_copy(base_rd, NULL)
                                      : yyjson_mut_doc_new(NULL);
        if (base_rd) yyjson_doc_free(base_rd);
        yyjson_mut_val* base = doc ? yyjson_mut_doc_get_root(doc) : nullptr;
        if (!base) { base = yyjson_mut_obj(doc); yyjson_mut_doc_set_root(doc, base); }

        // Pull patches from input (re-parse via JSON since Record doesn't
        // expose its value directly). Mutable-copy into `doc` so patch values
        // can be transplanted into base.
        std::string in_json = input.data_json();
        yyjson_doc* in_rd = yyjson_read(in_json.c_str(), in_json.size(), 0);
        yyjson_val* in_root = in_rd ? yyjson_doc_get_root(in_rd) : nullptr;
        if (in_root) {
            yyjson_mut_val* in = yyjson_val_mut_copy(doc, in_root);
            yyjson_mut_val* batch = yyjson_mut_obj_get(in, jkeys::kPatches);
            if (batch && yyjson_mut_is_arr(batch)) {
                size_t _i, _n; yyjson_mut_val* it;
                yyjson_mut_arr_foreach(batch, _i, _n, it) { apply_patch(doc, base, it); }
            } else if (yyjson_mut_obj_get(in, jkeys::kKey)) {
                apply_patch(doc, base, in);
            }
        }
        if (in_rd) yyjson_doc_free(in_rd);

        // Hand the built tree to a yyjson Record via a JSON round-trip.
        char* s = doc ? yyjson_mut_write(doc, 0, NULL) : nullptr;
        xi::Record result = s ? xi::Record::from_json_bytes((const uint8_t*)s, std::strlen(s))
                              : xi::Record();
        if (s) free(s);
        if (doc) yyjson_mut_doc_free(doc);
        return result;
    }

    std::string exchange(const std::string& cmd) override {
        yyjson_doc* doc = yyjson_read(cmd.c_str(), cmd.size(), 0);
        yyjson_val* p = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!p) { yyjson_doc_free(doc); return get_def(); }
        yyjson_val* c = yyjson_obj_get(p, jkeys::kCommand);
        yyjson_val* v = yyjson_obj_get(p, jkeys::kValue);
        if (c && yyjson_is_str(c)) {
            std::string command = yyjson_get_str(c);
            if (command == jkeys::kSetData) {
                // Guard 2: the payload is required — fail loud, don't silently
                // no-op on a set_data that forgot its "value".
                if (!v) {
                    yyjson_doc_free(doc);
                    return xi::contract::fault_json(xi::contract::kMissingInput, jkeys::kValue, "json");
                }
                char* s = yyjson_val_write(v, 0, NULL);
                if (s) { stored_json_.assign(s); free(s); }
            } else if (command == jkeys::kReset) {
                stored_json_ = "{}";
            }
            // get_status falls through to get_def() below
        }
        yyjson_doc_free(doc);
        return get_def();
    }

    std::string get_def() const override {
        // get_def returns wrapper { data: <stored> } so the UI knows the
        // value is the user JSON, not part of the def itself.
        yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val* root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);

        yyjson_doc* data_rd = yyjson_read(stored_json_.c_str(), stored_json_.size(), 0);
        yyjson_val* data_root = data_rd ? yyjson_doc_get_root(data_rd) : nullptr;
        yyjson_mut_val* data = data_root ? yyjson_val_mut_copy(doc, data_root)
                                         : yyjson_mut_obj(doc);
        yyjson_mut_obj_add_val(doc, root, jkeys::kData, data);

        char* s = yyjson_mut_write(doc, 0, NULL);
        std::string out = s ? s : "{\"data\":{}}";
        if (s) free(s);
        if (data_rd) yyjson_doc_free(data_rd);
        yyjson_mut_doc_free(doc);
        return out;
    }

    bool set_def(const std::string& json) override {
        yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
        yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!root) { yyjson_doc_free(doc); return false; }
        // Guard 3: reject a config built against an incompatible header schema
        // (a matching or absent stamp proceeds — legacy persisted defs have none).
        yyjson_val* sv = yyjson_obj_get(root, xi::contract::kSchemaKey);
        if (sv && yyjson_is_num(sv) &&
            (int)yyjson_get_num(sv) != xi::json_source::kSchemaVersion) {
            log_error(std::string("json_source: config schema mismatch: built for v") +
                      std::to_string((int)yyjson_get_num(sv)) + ", this plugin serves v" +
                      std::to_string(xi::json_source::kSchemaVersion));
            yyjson_doc_free(doc);
            return false;
        }
        yyjson_val* data = yyjson_obj_get(root, jkeys::kData);
        if (data) {
            char* s = yyjson_val_write(data, 0, NULL);
            if (s) { stored_json_.assign(s); free(s); }
        }
        yyjson_doc_free(doc);
        return true;
    }

private:
    std::string stored_json_ = "{}";
};

XI_PLUGIN_IMPL(JsonSource)
