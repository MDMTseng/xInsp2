//
// record_save.cpp — saves inspection results to disk. BILINGUAL (doc 10 gate P3):
//
//   * Record door  process(const Record&) — the legacy sink, UNTOUCHED: writes
//     <base>.json + one <base>_<key>.bmp per image, exactly as before.
//   * Pack door     process(PackIn&, PackOut&) — persists the sealed pack as the
//     CANONICAL XEX1-v2 dump: one <base>.xex1 file per capture whose bytes are the
//     shared encoder's output (xex1_pack_dump.hpp — the SAME encoder expose pushes
//     on the wire), so disk ≈ wire ≈ memory (doc 07). Replay reads it back through
//     xex1_pack_load.hpp (untrusted-disk ingress); see plugins/record_save/README.md.
//
// Configurable via UI:
//   - output_dir: where to save files
//   - naming_rule: pattern with {count}, {timestamp} placeholders
//   - enabled: toggle saving on/off
//

#include <xi/xi_abi.hpp>
#include "yyjson.h"   // parses def commands with yyjson
#include "stb_image_write.h"
#include "xex1_pack_dump.hpp"  // xi::xex1::encode_pack_v2 — the shared pack->v2 dump

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class RecordSave : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        xi::Record result;
        if (!enabled_) {
            result.set("saved", false);
            result.set("reason", "disabled");
            return result;
        }
        if (output_dir_.empty()) {
            result.set("saved", false);
            result.set("reason", "no output_dir set");
            return result;
        }

        std::filesystem::create_directories(output_dir_);

        // Generate filename from naming rule
        std::string base = render_filename(naming_rule_, ++save_count_);
        std::filesystem::path dir(output_dir_);

        // Save JSON metadata
        std::filesystem::path json_path = dir / (base + ".json");
        {
            std::ofstream f(json_path.string());
            f << input.data_json_pretty();
        }

        // Save each image
        int img_idx = 0;
        for (auto& [key, img] : input.images()) {
            if (img.empty()) continue;
            std::filesystem::path img_path = dir / (base + "_" + key + ".bmp");
            stbi_write_bmp(img_path.string().c_str(),
                          img.width, img.height, img.channels, img.data());
            img_idx++;
        }

        result.set("saved", true);
        result.set("count", save_count_);
        result.set("base_name", base);
        result.set("images_saved", img_idx);
        return result;
    }

    // Pack door (doc 10 gate P3): persist the sealed pack as ONE canonical
    // XEX1-v2 file per capture. The bytes are the shared encoder's output — the
    // exact frame expose pushes on the wire — so the persisted file IS the
    // canonical dump (memory ≈ wire ≈ disk, doc 07). The Record door above is
    // untouched; a project routes packs here to get the .xex1 dump instead of the
    // .json + .bmp pair. Returns a small ack pack mirroring the Record ack.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        if (!enabled_)          { out.boolean("saved", false).str("reason", "disabled"); return; }
        if (output_dir_.empty()) { out.boolean("saved", false).str("reason", "no output_dir set"); return; }

        // Reserved fields ride the frame header (lifted, not dumped as entries).
        const std::string channel(in.str(xi::Record::kChannelKey).value_or("default"));
        const uint64_t    seq = (uint64_t)in.i64_or(xi::Record::kSeqKey, 0);

        std::vector<uint8_t> frame = xi::xex1::encode_pack_v2(in, channel, seq);

        std::filesystem::create_directories(output_dir_);
        std::string base = render_filename(naming_rule_, ++save_count_);
        std::filesystem::path path = std::filesystem::path(output_dir_) / (base + ".xex1");
        {
            std::ofstream f(path.string(), std::ios::binary);
            f.write(reinterpret_cast<const char*>(frame.data()), (std::streamsize)frame.size());
        }

        out.boolean("saved", true)
           .i64("count", save_count_)
           .str("base_name", base + ".xex1")
           .str("format", "xex1.v2")
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
// Bilingual (doc 10 gate P3): publish the xi.pack@1 door so the host learns
// record_save consumes packs and persists them as the canonical XEX1-v2 dump.
XI_PLUGIN_PACK_DOOR(RecordSave)
