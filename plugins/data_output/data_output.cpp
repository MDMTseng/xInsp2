//
// data_output.cpp — saves inspection results to files.
//
// Configurable: output directory, format (csv/json), auto-save toggle.
//

#include <xi/xi_instance.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

class DataOutput : public xi::InstanceBase {
public:
    explicit DataOutput(std::string name)
        : name_(std::move(name)) {}

    const std::string& name() const override { return name_; }
    std::string plugin_name() const override { return "data_output"; }

    std::string get_def() const override {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"output_dir":"%s","format":"%s","auto_save":%s,"count":%d})",
            output_dir_.c_str(), format_.c_str(),
            auto_save_ ? "true" : "false", save_count_);
        return buf;
    }

    bool set_def(const std::string& j) override {
        auto extract_str = [&](const char* key) -> std::string {
            auto k = std::string("\"") + key + "\":\"";
            auto pos = j.find(k);
            if (pos == std::string::npos) return "";
            pos += k.size();
            auto end = j.find('"', pos);
            return end == std::string::npos ? "" : j.substr(pos, end - pos);
        };
        auto s = extract_str("output_dir");
        if (!s.empty()) output_dir_ = s;
        s = extract_str("format");
        if (!s.empty()) format_ = s;
        if (j.find("\"auto_save\":true") != std::string::npos) auto_save_ = true;
        if (j.find("\"auto_save\":false") != std::string::npos) auto_save_ = false;
        return true;
    }

    std::string exchange(const std::string& cmd_json) override {
        if (cmd_json.find("\"get_status\"") != std::string::npos) {
            return get_def();
        }
        if (cmd_json.find("\"set_output_dir\"") != std::string::npos) {
            set_def(cmd_json);
            return get_def();
        }
        if (cmd_json.find("\"save\"") != std::string::npos) {
            // In a real implementation, the inspection result would be
            // passed via the command. For now, just increment the counter.
            save_count_++;
            return get_def();
        }
        return R"({"error":"unknown command"})";
    }

private:
    std::string name_;
    std::string output_dir_ = "./output";
    std::string format_ = "csv";
    bool        auto_save_ = true;
    int         save_count_ = 0;
};

extern "C" __declspec(dllexport)
xi::InstanceBase* xi_plugin_create(const char* instance_name) {
    return new DataOutput(instance_name);
}
