//
// comm.cpp — example ORDERED-SINK plugin (a stand-in for a comm / PLC forwarder).
//
// THE POINT OF THIS EXAMPLE
// -------------------------
// Under parallel dispatch (project.json parallelism.dispatch_threads > 1) inspects
// finish OUT OF ORDER. A plugin that sends results to a PLC must still send them in
// FRAME order. The host makes that automatic for an instance whose plugin.json says
//
//     "sink": true
//
// The sink FEED is a script's
//
//     xi::use("comm0").push(pack)
//
// It is NOT run inline during the inspect. The host STAGES the sealed pack and
// flushes it AFTER the inspect, inside the ordered-emit gate — so deliveries to
// this plugin's xi.pack@1 door are serialized in frame (arrival) order, with no
// new script verb and no new ABI. (Request-reply process(pack) on a declared sink
// is refused at the funnel: a staged call's reply cannot exist mid-inspect — push
// is fire-and-forget, the door's reply pack is dropped.) A frame that is dropped
// (queue overflow) simply never arrives here — no head-of-line gap to wait on.
//
// Each delivered pack carries the reserved i64 entry "$seq" — the frame's arrival
// id (the same id on the run's wire result) — so a sink can correlate the packet
// to its frame and notice gaps. A real comm plugin would open its socket /
// fieldbus and forward the pack's entries here (walk them generically with
// in.for_each, or read known keys); this demo just records the $seq it saw, in
// order, and exposes it via exchange("get_received") so the ordering is observable.
//
// Build: `cmake -B build` here (see CMakeLists.txt), or copy this folder under
// <project>/plugins/comm/ to have the backend compile it as a project source plugin.
//
#include <xi/xi_abi.hpp>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

class Comm : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Called by the host's staged-sink flush, in frame order. `in` is the pack
    // the script pushed, plus the host-stamped "$seq". The reply pack is
    // dropped by the flush (fire-and-forget) — leave `out` untouched (an
    // untouched PackOut seals the empty absence sentinel).
    void process(xi::PackIn& in, xi::PackOut& /*out*/) override {
        const long long seq = in.i64_or(xi::pack_contract::kSeq, -1);   // frame arrival id

        {
            std::lock_guard<std::mutex> lk(mu_);
            received_.push_back(seq);
        }

        // A real comm plugin forwards to the line HERE — e.g. write `seq` + the
        // pack's data entries to a PLC register / socket / MES message. We just log it.
        std::fprintf(stderr, "[comm] forward frame seq=%lld\n", seq);
    }

    // get_received -> {received:[...], count}; reset -> clears.
    std::string exchange(const std::string& cmd) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (cmd.find("reset") != std::string::npos) {
            received_.clear();
            return "{\"ok\":true}";
        }
        std::string out = "{\"received\":[";
        for (size_t i = 0; i < received_.size(); ++i) {
            if (i) out += ",";
            out += std::to_string(received_[i]);
        }
        out += "],\"count\":" + std::to_string(received_.size()) + "}";
        return out;
    }

    std::string get_def() const override { return "{}"; }
    bool        set_def(const std::string&) override { return true; }

private:
    std::mutex             mu_;
    std::vector<long long> received_;
};

XI_PLUGIN_IMPL(Comm)
// Publish the xi.pack@1 door — the staged flush delivers each pushed pack
// through it. A sink still needs the door; only the FEED differs (push, not
// process). Always after XI_PLUGIN_IMPL.
XI_PLUGIN_PACK_DOOR(Comm)
