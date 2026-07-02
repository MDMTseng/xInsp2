#pragma once
//
// synced_stereo_io.h — the typed view over synced_stereo's config, commands, and
// the emitted stereo frame (the stage-1 deliverable of the plugin data contract).
//
// synced_stereo is a gathering source, so — like mock_camera — its builder targets
// the two string-carried surfaces a driver writes (config via set_def, commands
// via exchange) and its extractor reads the emitted record. The *_io.h convention
// is identical to mock_camera's: key names come from the generated
// synced_stereo_keys.gen.h (decl: contract/plugins/synced_stereo.decl.json), the
// Config builder stamps the schema version, and every accessor compiles down to
// plain Record / JSON operations — nothing new crosses the ABI.
//
//   #include "synced_stereo_io.h"
//
//   host.set_def(cam, xi::synced_stereo::Config().fps(30));
//   host.exchange(cam, xi::synced_stereo::Command::start());
//   host.exchange(cam, xi::synced_stereo::Command::fire(4));   // headless drive
//   ...
//   xi::synced_stereo::Frame f{ emitted_record };
//   if (f.has_both()) correlate(f.left(), f.right());
//

#include "synced_stereo_keys.gen.h"

#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_record.hpp>

#include <string>

namespace xi::synced_stereo {

// ---- Config builder (produces the set_def JSON) ----------------------------
class Config {
public:
    Config() { j_ = xi::Json::object().set(xi::contract::kSchemaKey, kSchemaVersion); }

    Config& fps(int v) { j_.set(keys::kFps, v); return *this; }

    std::string dump() const { return j_.dump(); }
    operator std::string() const { return j_.dump(); }

private:
    xi::Json j_;
};

// ---- Command builder (produces the exchange JSON) --------------------------
// One static factory per command; each returns the JSON string exchange() eats.
class Command {
public:
    static std::string start() { return cmd_(keys::kStart); }
    static std::string stop()  { return cmd_(keys::kStop); }
    static std::string set_fps(int v) {
        return xi::Json::object().set(keys::kCommand, keys::kSetFps)
                                 .set(keys::kValue, v).dump();
    }
    // Deterministic headless drive: emit `n` frames synchronously.
    static std::string fire(int n = 1) {
        return xi::Json::object().set(keys::kCommand, keys::kFire)
                                 .set(keys::kN, n).dump();
    }

private:
    static std::string cmd_(const char* name) {
        return xi::Json::object().set(keys::kCommand, name).dump();
    }
};

// ---- Frame extractor (reads an emitted record) -----------------------------
// The correlated pair rides ONE record, so both images are read from the same
// object — that colocation IS the sync guarantee synced_stereo provides.
class Frame {
public:
    explicit Frame(xi::Record rec) : rec_(std::move(rec)) {}

    bool has_left()  const { return rec_.has_image(keys::kLeft); }
    bool has_right() const { return rec_.has_image(keys::kRight); }
    bool has_both()  const { return has_left() && has_right(); }

    const xi::Image& left()  const { return rec_.get_image(keys::kLeft); }
    const xi::Image& right() const { return rec_.get_image(keys::kRight); }

    const xi::Record& record() const { return rec_; }

private:
    xi::Record rec_;
};

}  // namespace xi::synced_stereo
