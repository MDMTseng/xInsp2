#pragma once
// preview_api.hpp — script-side helper for the preview_sink plugin.
//
// Surface a Record to a named preview instance under a preview-group id (pg_id),
// so a UI can tab between groups (per stage / per thread / per camera / ...).
// The pg_id rides in the record under the reserved key "$pg" — the contract
// between this helper and the preview_sink plugin.
//
// xi::Record is already the "construct image + json in order" builder:
//   xi::Record().set("score", s).set("gain", g).image("edges", im)
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <string>
#include <utility>

namespace xi { namespace preview {

// Free form:  xi::preview::send("bright", xi::Record().set("score", s).image("img", im));
inline void send(const std::string& pg_id, xi::Record rec,
                 const std::string& inst = "preview") {
    rec.set("$pg", pg_id);
    xi::use(inst).process(rec);
}

// Object form matching the .process(pg_id, rec) sketch:
//   xi::preview::Sink pv;  pv.process("bright", rec);  pv.process("dark", rec2);
class Sink {
public:
    explicit Sink(std::string inst = "preview") : inst_(std::move(inst)) {}
    void process(const std::string& pg_id, xi::Record rec) const {
        send(pg_id, std::move(rec), inst_);
    }
private:
    std::string inst_;
};

}}  // namespace xi::preview
