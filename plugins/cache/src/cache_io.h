#pragma once
//
// cache_io.h — the typed view over the cache (BufferReplay) plugin's command /
// config / capture-ack surface (the stage-1 deliverable of the plugin data
// contract).
//
// The buffered records themselves are an OPEN schema (cache replays anything),
// so there is intentionally no typed view over the payload. This header types
// the fixed CONTROL vocabulary a driver writes: the replay commands + their
// params, the ring config, and the small process() acknowledgement. Every
// builder/extractor compiles down to plain Record / JSON — nothing new crosses
// the ABI.
//
//   #include "cache_io.h"
//
//   host.set_def(buf, xi::cache::Config().capacity(32));
//   host.exchange(buf, xi::cache::Command::replay_last(1));   // hot-param re-inspect
//   host.exchange(buf, xi::cache::Command::replay_timed(2.0)); // paced x2
//   auto st = xi::cache::Status{ host.get_def(buf) };
//   assert(st.count() <= st.capacity());
//

#include "cache_keys.h"

#include <xi/xi_contract.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_record.hpp>

#include <string>

namespace xi::cache {

// ---- Config builder (produces the set_def JSON) ----------------------------
class Config {
public:
    Config() { j_ = xi::Json::object().set(xi::contract::kSchemaKey, kSchemaVersion); }

    Config& capacity(int v) { j_.set(keys::kCapacity, v); return *this; }

    std::string dump() const { return j_.dump(); }
    operator std::string() const { return j_.dump(); }

private:
    xi::Json j_;
};

// ---- Command builder (produces the exchange JSON) --------------------------
class Command {
public:
    static std::string replay_last(int n = 1) {
        return xi::Json::object().set(keys::kCommand, keys::kReplayLast)
                                 .set(keys::kN, n).dump();
    }
    static std::string replay_all()  { return cmd_(keys::kReplayAll); }
    static std::string replay(int index) {
        return xi::Json::object().set(keys::kCommand, keys::kReplay)
                                 .set(keys::kIndex, index).dump();
    }
    static std::string replay_timed(double speed = 1.0, int n = -1) {
        auto j = xi::Json::object().set(keys::kCommand, keys::kReplayTimed)
                                   .set(keys::kSpeed, speed);
        if (n > 0) j.set(keys::kN, n);
        return j.dump();
    }
    static std::string stop_replay() { return cmd_(keys::kStopReplay); }
    static std::string clear()       { return cmd_(keys::kClear); }
    static std::string set_capacity(int v) {
        return xi::Json::object().set(keys::kCommand, keys::kSetCapacity)
                                 .set(keys::kValue, v).dump();
    }

private:
    static std::string cmd_(const char* name) {
        return xi::Json::object().set(keys::kCommand, name).dump();
    }
};

// ---- Capture acknowledgement (reads the process() output Record) -----------
// process() returns {"buffered": <ring size>}. The buffered payload itself is
// open — this reads only the ack the plugin stamps.
class Capture {
public:
    explicit Capture(xi::Record rec) : rec_(std::move(rec)) {}
    int buffered() const { return rec_.get_int(keys::kBuffered); }
    const xi::Record& record() const { return rec_; }
private:
    xi::Record rec_;
};

// ---- Status extractor (reads the get_def reply) ----------------------------
class Status {
public:
    explicit Status(const std::string& json) : j_(xi::Json::parse(json)) {}
    bool valid() const { return j_.valid(); }
    int  capacity()  const { return j_[keys::kCapacity].as_int(); }
    int  count()     const { return j_[keys::kCount].as_int(); }   // TOTAL ring occupancy (records + packs)
    int  packs()     const { return j_[keys::kPacks].as_int(); }   // of which are retained sealed packs
    bool replaying() const { return j_[keys::kReplaying].as_bool(); }
private:
    xi::Json j_;
};

}  // namespace xi::cache
