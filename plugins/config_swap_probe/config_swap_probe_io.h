#pragma once
//
// config_swap_probe_io.h — the typed view over config_swap_probe's config,
// command, and status (the stage-1 deliverable of the plugin data contract).
//
// The probe's process() is a no-op (no input contract, empty output), so there
// is no Input/Output pair. What it exposes is a source-style control surface: a
// Config builder (the {value} the double-slot loads), a Command builder for
// get_status, and a Status extractor over the reply. Every accessor compiles down
// to plain JSON — nothing new crosses the ABI.
//
//   #include "config_swap_probe_io.h"
//
//   host.set_def(p, xi::config_swap_probe::Config().value(42));
//   host.prepare(p, xi::config_swap_probe::Config().value(99), folder);  // stage
//   ...
//   auto s = xi::config_swap_probe::Status{ host.exchange(p, Command::get_status()) };
//   assert(s.active() == 42 && s.has_staged() && s.staged_value() == 99);
//

#include "config_swap_probe_keys.gen.h"

#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>

#include <string>

namespace xi::config_swap_probe {

// ---- Config builder (produces the set_def / prepare JSON) ------------------
class Config {
public:
    Config() { j_ = xi::Json::object().set(xi::contract::kSchemaKey, kSchemaVersion); }

    Config& value(int v) { j_.set(keys::kValue, v); return *this; }

    std::string dump() const { return j_.dump(); }
    operator std::string() const { return j_.dump(); }

private:
    xi::Json j_;
};

// ---- Command builder (produces the exchange JSON) --------------------------
class Command {
public:
    static std::string get_status() {
        return xi::Json::object().set(keys::kCommand, keys::kGetStatus).dump();
    }
};

// ---- Status extractor (reads the get_status reply) -------------------------
class Status {
public:
    explicit Status(const std::string& json) : j_(xi::Json::parse(json)) {}

    bool valid() const { return j_.valid(); }

    int  active()       const { return j_[keys::kActive].as_int(); }
    bool has_staged()   const { return j_[keys::kStaged].as_bool(); }
    int  staged_value() const { return j_[keys::kStagedValue].as_int(); }
    int  last_seen()    const { return j_[keys::kLastSeen].as_int(); }
    long long proc()    const { return (long long)j_[keys::kProc].as_double(); }

private:
    xi::Json j_;
};

}  // namespace xi::config_swap_probe
