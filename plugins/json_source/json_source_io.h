#pragma once
//
// json_source_io.h — the typed view over json_source's CONTROL surface (config,
// commands, and the process-input patch shape). Stage-1 deliverable of the
// plugin data contract.
//
// json_source EMITS user-authored JSON, so its output is an OPEN schema — there
// is intentionally NO Output extractor here (the produced record is whatever the
// user wrote). What this header types is the fixed vocabulary a DRIVER writes:
// the config wrapper, the exchange commands, and the runtime patch shape. Every
// builder compiles down to a plain JSON string / Record — nothing new crosses
// the ABI, exactly like mock_camera's source-side builders.
//
//   #include "json_source_io.h"
//
//   host.set_def(src, xi::json_source::Config().data(R"({"roi":[0,0,64,64]})"));
//   host.exchange(src, xi::json_source::Command::set_data(R"({"n":3})"));
//   // per-emit patch (open value): bump one path for THIS emit only
//   auto in = xi::json_source::Patch::single(".n", "7");
//
// The `value` payloads are arbitrary JSON (that openness is the plugin's whole
// point), so the builders take pre-serialized JSON text rather than typed
// scalars — the one place the typed view stays deliberately string-shaped.
//

#include "json_source_keys.h"

#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>

#include <initializer_list>
#include <string>
#include <utility>

namespace xi::json_source {

// ---- Config builder (produces the set_def JSON) ----------------------------
// { "_schema": N, "data": <raw user json> }. `data` is arbitrary JSON text, so
// it is concatenated rather than routed through a typed setter.
class Config {
public:
    Config& data(const std::string& raw_json) { data_ = raw_json; return *this; }

    std::string dump() const {
        return std::string("{\"") + xi::contract::kSchemaKey + "\":" +
               std::to_string(kSchemaVersion) + ",\"" + keys::kData + "\":" + data_ + "}";
    }
    operator std::string() const { return dump(); }

private:
    std::string data_ = "{}";
};

// ---- Command builder (produces the exchange JSON) --------------------------
class Command {
public:
    static std::string reset()      { return cmd_(keys::kReset); }
    static std::string get_status() { return cmd_(keys::kGetStatus); }
    // Replace the stored JSON. Payload is arbitrary JSON text.
    static std::string set_data(const std::string& raw_json) {
        return std::string("{\"") + keys::kCommand + "\":\"" + keys::kSetData +
               "\",\"" + keys::kValue + "\":" + raw_json + "}";
    }

private:
    static std::string cmd_(const char* name) {
        return xi::Json::object().set(keys::kCommand, name).dump();
    }
};

// ---- Patch builder (produces the process() input JSON) ---------------------
// The process input mutates the stored JSON for THIS emit only. Values are open
// JSON text.
class Patch {
public:
    // { "key": <path>, "value": <raw json> }
    static std::string single(const std::string& path, const std::string& raw_value) {
        return std::string("{\"") + keys::kKey + "\":\"" + path + "\",\"" +
               keys::kValue + "\":" + raw_value + "}";
    }
    // { "patches": [ {key,value}, ... ] }
    static std::string batch(std::initializer_list<std::pair<std::string, std::string>> patches) {
        std::string out = std::string("{\"") + keys::kPatches + "\":[";
        bool first = true;
        for (auto& [path, raw_value] : patches) {
            if (!first) out += ",";
            out += single(path, raw_value);
            first = false;
        }
        out += "]}";
        return out;
    }
};

}  // namespace xi::json_source
