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
#include <xi/xi_mp.hpp>         // canonical-profile msgpack Writer (nested JSON → mp)
#include <xi/xi_ingress.hpp>    // the foreign-bytes domain edge (doc 07 Ingress)
#include "yyjson.h"   // this plugin does raw yyjson path-building internally

#include "json_source_keys.gen.h"   // guard 1: command/config/patch key names, once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

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

// --- pack-mode (polaris2 wave-2) nested-value encoder -----------------------
// Depth cap for the JSON→msgpack walk. Matches the ingress limit so the pack
// interior never sees a value the edge would reject; also bounds THIS recursion
// so a depth-bomb document can't overflow the plugin's stack before the ingress
// canonicalizer ever runs (yyjson's reader is iterative, but this tree walk is
// not). The re-validation in xi::ingress uses the same bound.
inline constexpr int kPackMaxDepth = xi::mp::kDefaultMaxDepth;   // 64

// Encode one yyjson value into `w` in the canonical max-width profile. Returns
// false (aborting the whole emit) only on a depth-bomb — everything else is a
// well-formed canonical value. This is the "foreign JSON → msgpack" half; the
// bytes it produces are then re-proven by xi::ingress::canonicalize_entry.
bool encode_mut(xi::mp::Writer& w, yyjson_mut_val* v, int depth) {
    if (depth > kPackMaxDepth) return false;
    switch (yyjson_mut_get_type(v)) {
        case YYJSON_TYPE_NULL: w.nil();                       return true;
        case YYJSON_TYPE_BOOL: w.boolean(yyjson_mut_get_bool(v)); return true;
        case YYJSON_TYPE_NUM:
            switch (yyjson_mut_get_subtype(v)) {
                case YYJSON_SUBTYPE_UINT: w.uint_(yyjson_mut_get_uint(v)); break;
                case YYJSON_SUBTYPE_SINT: w.int_(yyjson_mut_get_sint(v));  break;
                default:                  w.float_(yyjson_mut_get_real(v)); break;
            }
            return true;
        case YYJSON_TYPE_STR:
            w.str(std::string_view(yyjson_mut_get_str(v), yyjson_mut_get_len(v)));
            return true;
        case YYJSON_TYPE_ARR: {
            w.array((uint32_t)yyjson_mut_arr_size(v));
            size_t i, n; yyjson_mut_val* e;
            yyjson_mut_arr_foreach(v, i, n, e)
                if (!encode_mut(w, e, depth + 1)) return false;
            return true;
        }
        case YYJSON_TYPE_OBJ: {
            w.map((uint32_t)yyjson_mut_obj_size(v));
            size_t i, n; yyjson_mut_val *k, *val;
            yyjson_mut_obj_foreach(v, i, n, k, val) {
                w.key(std::string_view(yyjson_mut_get_str(k), yyjson_mut_get_len(k)));
                if (!encode_mut(w, val, depth + 1)) return false;
            }
            return true;
        }
    }
    return false;
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

        // polaris2 wave-2 (bilingual): when pack_mode is set AND the host
        // publishes xi.pack@1, emit the (patched) document as a sealed Pack
        // through the host door instead of returning a Record — the same emit
        // path mock_camera takes. The Record output plane is left empty; the
        // payload rides the pack. Default OFF, so the Record path below is
        // byte-for-byte unchanged for every existing project.
        if (pack_mode_.load() && pack_iface()) {
            emit_pack_(base);
            if (doc) yyjson_mut_doc_free(doc);
            return xi::Record();
        }

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
        // Reflect the pack-mode flag so a UI/driver can read (and round-trip
        // via set_def) the emit mode — mirrors mock_camera's get_def.
        yyjson_mut_obj_add_bool(doc, root, jkeys::kPackMode, pack_mode_.load());

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
        // polaris2 wave-2: opt into pack-plane emit (default false). An absent
        // key leaves the current mode (a legacy config keeps emitting Records).
        yyjson_val* pm = yyjson_obj_get(root, jkeys::kPackMode);
        if (pm && yyjson_is_bool(pm)) pack_mode_ = yyjson_get_bool(pm);
        yyjson_doc_free(doc);
        return true;
    }

private:
    // Build a sealed Pack from the (patched) document tree `base` and emit it via
    // the host xi.pack@1 door. Mapping rule (documented in the plugin README):
    //   * a `seq` i64 entry (emit counter) leads, mirroring mock_camera;
    //   * each TOP-LEVEL scalar field becomes a canonical entry (bool → i64 0/1,
    //     since the pack scalar plane has no bool tag; number → i64/f64;
    //     string → str; null is skipped);
    //   * each TOP-LEVEL nested value (object/array) becomes ONE `mp` entry whose
    //     bytes are produced by encode_mut and then RE-PROVEN through
    //     xi::ingress::canonicalize_entry — the user JSON is FOREIGN input, so it
    //     crosses into the pack plane only through the domain edge (doc 07).
    // A user field literally named "seq" is reserved (shadowed by the counter).
    // Any rejected input (depth bomb, oversized, non-object root) emits a sealed
    // $fault pack (fail loud) rather than a partial or silent result.
    void emit_pack_(yyjson_mut_val* base) {
        xi::PackOut f = new_pack();
        if (!f.valid()) return;   // no pack plane (should not happen: caller checked)

        const int64_t seq = seq_.fetch_add(1);
        f.i64(jkeys::kSeq, seq);

        const char* err_code = nullptr;
        const char* err_key  = nullptr;
        std::string err_detail;

        if (!yyjson_mut_is_obj(base)) {
            err_code = xi::contract::kWrongType;
            err_key  = jkeys::kData;
            err_detail = "pack_mode requires a JSON object at the document root";
        } else {
            size_t i, n; yyjson_mut_val *k, *v;
            yyjson_mut_obj_foreach(base, i, n, k, v) {
                const char* key = yyjson_mut_get_str(k);
                if (!key) continue;
                if (std::strcmp(key, jkeys::kSeq) == 0) continue;  // reserved
                if (!add_field_(f, key, v, err_code, err_key, err_detail)) break;
            }
        }

        if (err_code) { emit_fault_(seq, err_code, err_key, err_detail); return; }
        emit(std::move(f));   // seals + hands to host dispatch (drops our ref)
    }

    // Add one top-level field to the pack. Returns false (and fills err_*) when
    // a nested value fails the ingress edge, so the caller can fail loud.
    bool add_field_(xi::PackOut& f, const char* key, yyjson_mut_val* v,
                    const char*& ec, const char*& ek, std::string& ed) {
        switch (yyjson_mut_get_type(v)) {
            case YYJSON_TYPE_NULL: return true;   // documented: null is skipped
            case YYJSON_TYPE_BOOL: f.boolean(key, yyjson_mut_get_bool(v)); return true;
            case YYJSON_TYPE_NUM:
                switch (yyjson_mut_get_subtype(v)) {
                    case YYJSON_SUBTYPE_UINT: f.i64(key, (int64_t)yyjson_mut_get_uint(v)); break;
                    case YYJSON_SUBTYPE_SINT: f.i64(key, yyjson_mut_get_sint(v)); break;
                    default:                  f.f64(key, yyjson_mut_get_real(v)); break;
                }
                return true;
            case YYJSON_TYPE_STR:
                f.str(key, std::string_view(yyjson_mut_get_str(v), yyjson_mut_get_len(v)));
                return true;
            case YYJSON_TYPE_ARR:
            case YYJSON_TYPE_OBJ: {
                xi::mp::Writer w;
                if (!encode_mut(w, v, 1)) {   // depth bomb aborted the walk
                    ec = xi::mp::status_str(xi::mp::Status::DepthExceeded);
                    ek = key; ed = "nested JSON exceeds the ingress depth limit";
                    return false;
                }
                xi::mp::Bytes raw = w.take();
                if (raw.size() > kPackMaxEntryBytes) {
                    ec = "oversized"; ek = key;
                    ed = "nested entry exceeds the ingress size cap";
                    return false;
                }
                // The foreign edge: prove structure + normalize to the profile in
                // one pass. (Idempotent here since encode_mut already emitted the
                // canonical forms, but this is the ONLY sanctioned foreign→pack
                // path and it re-checks depth/dup-keys/trailing bytes.)
                xi::ingress::Options opts;
                opts.max_depth = kPackMaxDepth;
                xi::ingress::Result r = xi::ingress::canonicalize_entry(raw, "mp", opts);
                if (!r.ok()) {
                    ec = xi::mp::status_str(r.codec_status);
                    ek = key;
                    ed = "nested payload rejected at the pack ingress edge";
                    return false;
                }
                f.mp(key, r.canonical.data(), r.canonical.size());
                return true;
            }
        }
        ec = xi::contract::kWrongType; ek = key; ed = "unsupported JSON value type";
        return false;
    }

    // A contract failure is a NORMAL sealed pack carrying seq + the $fault stamp
    // (pilot convention) so the consumer always gets a pack to route to a verdict.
    void emit_fault_(int64_t seq, const char* code, const char* key,
                     const std::string& detail) {
        xi::PackOut f = new_pack();
        if (!f.valid()) return;
        f.i64(jkeys::kSeq, seq);
        f.fault(code, key, detail);
        emit(std::move(f));
    }

    // A nested entry above this many encoded bytes is rejected as oversized.
    static constexpr size_t kPackMaxEntryBytes = 8u * 1024u * 1024u;

    std::string stored_json_ = "{}";
    std::atomic<bool>    pack_mode_{false};   // polaris2 wave-2 (default OFF)
    std::atomic<int64_t> seq_{0};             // per-emit counter (pack mode)
};

XI_PLUGIN_IMPL(JsonSource)
