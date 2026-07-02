#pragma once
//
// mock_camera_io.h — the typed view over mock_camera's config, commands, and
// emitted frame (the stage-1 deliverable of the plugin data contract).
//
// mock_camera is a source, so — unlike an operator plugin's Input/Output pair —
// its builder targets the two string-carried surfaces a driver actually writes
// (config via set_def, commands via exchange) and its extractor reads the
// emitted output frame. The *_io.h convention is otherwise identical to
// blob_analysis_io.h: key names come from mock_camera_keys.h, the builder stamps
// the schema version, and every accessor compiles down to plain Record / JSON
// operations — nothing new crosses the ABI.
//
//   #include "mock_camera_io.h"
//
//   host.set_def(cam, xi::mock_camera::Config().width(1280).height(720).fps(15));
//   host.exchange(cam, xi::mock_camera::Command::set_fps(30));
//   ...
//   xi::mock_camera::Frame f{ emitted_record };
//   if (f.has_frame()) show(f.image());
//

#include "mock_camera_keys.h"

#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_record.hpp>

#include <string>

namespace xi::mock_camera {

// ---- Config builder (produces the set_def JSON) ----------------------------
class Config {
public:
    Config() { j_ = xi::Json::object().set(xi::contract::kSchemaKey, kSchemaVersion); }

    Config& width(int v)  { j_.set(keys::kWidth, v);  return *this; }
    Config& height(int v) { j_.set(keys::kHeight, v); return *this; }
    Config& fps(int v)    { j_.set(keys::kFps, v);    return *this; }

    std::string dump() const { return j_.dump(); }
    operator std::string() const { return j_.dump(); }

private:
    xi::Json j_;
};

// ---- Command builder (produces the exchange JSON) --------------------------
// One static factory per command; each returns the JSON string exchange() eats.
class Command {
public:
    static std::string start()          { return cmd_(keys::kStart); }
    static std::string stop()           { return cmd_(keys::kStop); }
    static std::string get_status()     { return cmd_(keys::kGetStatus); }
    static std::string set_fps(int v) {
        return xi::Json::object().set(keys::kCommand, keys::kSetFps)
                                 .set(keys::kValue, v).dump();
    }
    static std::string set_resolution(int w, int h) {
        return xi::Json::object().set(keys::kCommand, keys::kSetResolution)
                                 .set(keys::kWidth, w).set(keys::kHeight, h).dump();
    }

private:
    static std::string cmd_(const char* name) {
        return xi::Json::object().set(keys::kCommand, name).dump();
    }
};

// ---- Frame extractor (reads an emitted output Record) ----------------------
class Frame {
public:
    explicit Frame(xi::Record rec) : rec_(std::move(rec)) {}

    bool has_frame() const { return rec_.has_image(keys::kFrame); }
    const xi::Image& image() const { return rec_.get_image(keys::kFrame); }

    const xi::Record& record() const { return rec_; }

private:
    xi::Record rec_;
};

}  // namespace xi::mock_camera
