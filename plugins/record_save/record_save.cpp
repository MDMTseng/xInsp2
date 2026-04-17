//
// record_save.cpp — saves inspection results to disk.
//
// Configurable via UI:
//   - output_dir: where to save files
//   - naming_rule: pattern with {count}, {timestamp} placeholders
//   - enabled: toggle saving on/off
//
// process(input) writes input.json + each input image to output_dir.
//

#include <xi/xi_abi.hpp>
#include "stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

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

    std::string exchange(const std::string& cmd) override {
        cJSON* p = cJSON_Parse(cmd.c_str());
        if (!p) return get_def();
        cJSON* c = cJSON_GetObjectItem(p, "command");
        cJSON* v = cJSON_GetObjectItem(p, "value");
        if (c && cJSON_IsString(c)) {
            std::string command = c->valuestring;
            if (command == "set_output_dir" && v && cJSON_IsString(v)) {
                output_dir_ = v->valuestring;
            } else if (command == "set_naming_rule" && v && cJSON_IsString(v)) {
                naming_rule_ = v->valuestring;
            } else if (command == "set_enabled" && v) {
                enabled_ = cJSON_IsTrue(v);
            } else if (command == "reset_count") {
                save_count_ = 0;
            }
        }
        cJSON_Delete(p);
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
        cJSON* p = cJSON_Parse(json.c_str());
        if (!p) return false;
        cJSON* od = cJSON_GetObjectItem(p, "output_dir"); if (od && cJSON_IsString(od)) output_dir_ = od->valuestring;
        cJSON* nr = cJSON_GetObjectItem(p, "naming_rule"); if (nr && cJSON_IsString(nr)) naming_rule_ = nr->valuestring;
        cJSON* en = cJSON_GetObjectItem(p, "enabled"); if (en) enabled_ = cJSON_IsTrue(en);
        cJSON_Delete(p);
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
