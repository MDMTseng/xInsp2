//
// record_save.cpp — saves inspection results to disk (doc 10 gate P3):
//
//   * Pack door  process(PackIn&, PackOut&) — persists the sealed pack as the
//     CANONICAL XEX1-v3 dump: one <base>.xex1 file per capture whose bytes are the
//     shared encoder's output (xex1_pack_dump.hpp — the SAME encoder expose pushes
//     on the wire), so disk ≈ wire ≈ memory (doc 07). Replay reads it back through
//     xex1_pack_load.hpp (untrusted-disk ingress); see toolbox/record_save/README.md.
//
// (v12: the legacy Record door that wrote <base>.json + <base>_<key>.bmp is
// deleted along with the Record data plane — the .xex1 pack dump is the sink.)
//
// Configurable via UI:
//   - output_dir: where to save files
//   - naming_rule: pattern with {count}, {timestamp} placeholders
//   - enabled: toggle saving on/off
//

#include <xi/xi_abi.hpp>
#include <xi/xi_atomic_io.hpp>      // xi::atomic_write — temp+rename; no truncated .xex1 behind saved:true
#include <xi/xi_pack_contract.hpp>  // reserved keys: xi::pack_contract::kChannel/kSeq
#include "yyjson.h"   // parses def commands with yyjson
#include "xex1_pack_dump.hpp"  // xi::xex1::encode_pack_v3 — the shared pack->v3 dump

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

class RecordSave : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Pack door (doc 10 gate P3) — THE data plane (v12): persist the sealed pack
    // as ONE canonical XEX1-v3 file per capture. The bytes are the shared
    // encoder's output — the exact frame expose pushes on the wire — so the
    // persisted file IS the canonical dump (memory ≈ wire ≈ disk, doc 07).
    // Returns a small ack pack.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        if (!enabled_)          { out.boolean("saved", false).str("reason", "disabled"); return; }
        if (output_dir_.empty()) { out.boolean("saved", false).str("reason", "no output_dir set"); return; }

        // Reserved fields ride the frame header (lifted, not dumped as entries).
        // F5: only lift them when the pack ACTUALLY carried them — else a pack
        // that never had $channel/$seq would be persisted with a spurious
        // "default"/0 header and gain those two entries on replay (count drift).
        // Presence is reflected into the header so replay re-injects them iff
        // they were there.
        const bool has_channel = in.has(xi::pack_contract::kChannel);
        const bool has_seq     = in.has(xi::pack_contract::kSeq);
        const std::string channel(in.str(xi::pack_contract::kChannel).value_or(""));
        const uint64_t    seq = (uint64_t)in.i64_or(xi::pack_contract::kSeq, 0);

        std::vector<uint8_t> frame =
            xi::xex1::encode_pack_v3(in, channel, seq, has_channel, has_seq);

        std::error_code dir_ec;
        std::filesystem::create_directories(output_dir_, dir_ec);
        if (dir_ec) {
            out.boolean("saved", false)
               .str("reason", "create_directories failed: " + dir_ec.message())
               .str("output_dir", output_dir_);
            return;
        }

        // GUARD — path containment. render_filename copies non-token chars
        // verbatim and fs::operator/ REPLACES the left side when the right is
        // absolute, so an operator-supplied naming_rule like "../x" (or an
        // absolute base) would write OUTSIDE output_dir_. Root cause: no
        // sanitation of naming_rule/output_dir at config-set time
        // (set_naming_rule / set_def); a fuller fix would validate/reject
        // there. Until then: refuse any rendered base that could leave the
        // directory and fail the save LOUD (saved:false + reason) rather than
        // write outside. With separators, "..", and ':' rejected, base is a
        // single lexical filename, so output_dir_/base cannot escape.
        const int next_count = save_count_ + 1;   // advance ONLY after a successful write
        std::string base = render_filename(naming_rule_, next_count);
        if (base.empty()
            || base.find('/')  != std::string::npos
            || base.find('\\') != std::string::npos
            || base.find("..") != std::string::npos
            || base.find(':')  != std::string::npos) {
            out.boolean("saved", false)
               .str("reason", "naming_rule renders unsafe base name (empty, path separator, '..' or ':'): \"" + base + "\"");
            return;
        }
        std::filesystem::path path = std::filesystem::path(output_dir_) / (base + ".xex1");

        // Atomic + checked: the plain ofstream write here was unchecked, so
        // disk-full / IO error shipped saved:true over a truncated or empty
        // file. atomic_write stages to <path>.tmp, flushes, and renames —
        // readers never observe a partial .xex1, and a failed write is
        // reported honestly (saved:false, count NOT advanced).
        const std::string_view bytes(reinterpret_cast<const char*>(frame.data()), frame.size());
        if (!xi::atomic_write(path, bytes)) {
            out.boolean("saved", false)
               .str("reason", "write failed (disk full / IO error): " + path.string())
               .i64("bytes", (int64_t)frame.size());
            return;
        }
        save_count_ = next_count;

        out.boolean("saved", true)
           .i64("count", save_count_)
           .str("base_name", base + ".xex1")
           .str("format", "xex1.v3")
           .i64("bytes", (int64_t)frame.size());
    }

    std::string exchange(const std::string& cmd) override {
        yyjson_doc* doc = yyjson_read(cmd.c_str(), cmd.size(), 0);
        yyjson_val* p = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!p) { yyjson_doc_free(doc); return get_def(); }
        yyjson_val* c = yyjson_obj_get(p, "command");
        yyjson_val* v = yyjson_obj_get(p, "value");
        if (c && yyjson_is_str(c)) {
            std::string command = yyjson_get_str(c);
            if (command == "set_output_dir" && v && yyjson_is_str(v)) {
                output_dir_ = yyjson_get_str(v);
            } else if (command == "set_naming_rule" && v && yyjson_is_str(v)) {
                naming_rule_ = yyjson_get_str(v);
            } else if (command == "set_enabled" && v) {
                enabled_ = yyjson_get_bool(v);
            } else if (command == "reset_count") {
                save_count_ = 0;
            }
        }
        yyjson_doc_free(doc);
        return get_def();
    }

    std::string get_def() const override {
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            R"({"output_dir":"%s","naming_rule":"%s","enabled":%s,"count":%d})",
            escape_for_json(output_dir_).c_str(),
            escape_for_json(naming_rule_).c_str(),
            enabled_ ? "true" : "false",
            save_count_);
        return buf;
    }

    bool set_def(const std::string& json) override {
        yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
        yyjson_val* p = doc ? yyjson_doc_get_root(doc) : nullptr;
        if (!p) { yyjson_doc_free(doc); return false; }
        yyjson_val* od = yyjson_obj_get(p, "output_dir"); if (od && yyjson_is_str(od)) output_dir_ = yyjson_get_str(od);
        yyjson_val* nr = yyjson_obj_get(p, "naming_rule"); if (nr && yyjson_is_str(nr)) naming_rule_ = yyjson_get_str(nr);
        yyjson_val* en = yyjson_obj_get(p, "enabled"); if (en) enabled_ = yyjson_get_bool(en);
        yyjson_doc_free(doc);
        return true;
    }

private:
    std::string output_dir_;
    std::string naming_rule_ = "frame_{count}";
    bool enabled_ = false;
    int save_count_ = 0;

    static std::string render_filename(const std::string& pattern, int count) {
        std::string out;
        for (size_t i = 0; i < pattern.size(); ++i) {
            if (pattern[i] == '{') {
                auto e = pattern.find('}', i);
                if (e == std::string::npos) { out.push_back(pattern[i]); continue; }
                std::string token = pattern.substr(i + 1, e - i - 1);
                if (token == "count") {
                    char nb[16]; std::snprintf(nb, sizeof(nb), "%06d", count);
                    out += nb;
                } else if (token == "timestamp") {
                    auto now = std::chrono::system_clock::now();
                    auto t = std::chrono::system_clock::to_time_t(now);
                    char tb[32]; std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", std::localtime(&t));
                    out += tb;
                } else {
                    out += "{" + token + "}";
                }
                i = e;
            } else {
                out.push_back(pattern[i]);
            }
        }
        return out;
    }

    static std::string escape_for_json(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == '"' || c == '\\') { out.push_back('\\'); }
            if (c == '\n') { out += "\\n"; continue; }
            out.push_back(c);
        }
        return out;
    }
};

XI_PLUGIN_IMPL(RecordSave)
// Publish the xi.pack@1 door (doc 10 gate P3) so the host learns record_save
// consumes packs and persists them as the canonical XEX1-v3 dump.
XI_PLUGIN_PACK_DOOR(RecordSave)
