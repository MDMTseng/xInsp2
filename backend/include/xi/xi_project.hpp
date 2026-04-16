#pragma once
//
// xi_project.hpp — project.json save/load for xInsp2.
//
// A project file stores:
//   - param values (name → scalar)
//   - instance defs (name → JSON object)
//
// Format is a plain JSON object:
//
//   {
//     "params": { "sigma": 3.5, "low": 60 },
//     "instances": { "cam0": { ... }, "template": { ... } }
//   }
//
// The service serializes from / deserializes to the script DLL's own
// registries via the thunk functions.
//

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace xi::project {

inline std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline bool write_text(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(content.data(), (std::streamsize)content.size());
    return f.good();
}

// Build project.json content from the script's thunk outputs.
// params_json: JSON array from xi_script_list_params, e.g. [{"name":"sigma","type":"float","value":3.5,...}]
// instances_json: JSON array from xi_script_list_instances, e.g. [{"name":"cam0","plugin":"Camera","def":{...}}]
inline std::string build_project_json(const std::string& params_json,
                                       const std::string& instances_json) {
    std::string out = "{\n  \"params\": ";
    out += params_json.empty() ? "[]" : params_json;
    out += ",\n  \"instances\": ";
    out += instances_json.empty() ? "[]" : instances_json;
    out += "\n}\n";
    return out;
}

} // namespace xi::project
