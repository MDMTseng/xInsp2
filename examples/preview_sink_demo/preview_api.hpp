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

// --- pvar: append a value or image to a preview record, in order -------------
//
// pvar(rec, "score", blobCount);     // a value
// pvar(rec, "edges", edgeImage);     // an image (auto-tagged)
//
// Each call appends {key, kind} to the record's "$layout" array, so a UI can walk
// the record IN CALL ORDER — read data[key] for a "value", fetch the image[key]
// for an "image" — instead of guessing from the bare data object (image keys don't
// live in the data JSON, so order + kind would otherwise be lost).
namespace detail {
inline void layout_push(xi::Record& rec, const std::string& key, const char* kind, int line) {
    rec.push("$layout", xi::Record().set("key", key)
                                    .set("kind", std::string(kind))
                                    .set("line", line));   // source line of the pvar() call
}
}  // namespace detail

inline xi::Record& append(xi::Record& rec, const std::string& key, int v, int line)
    { rec.set(key, v);              detail::layout_push(rec, key, "value", line); return rec; }
inline xi::Record& append(xi::Record& rec, const std::string& key, double v, int line)
    { rec.set(key, v);              detail::layout_push(rec, key, "value", line); return rec; }
inline xi::Record& append(xi::Record& rec, const std::string& key, bool v, int line)
    { rec.set(key, v);              detail::layout_push(rec, key, "value", line); return rec; }
inline xi::Record& append(xi::Record& rec, const std::string& key, const std::string& v, int line)
    { rec.set(key, v);              detail::layout_push(rec, key, "value", line); return rec; }
inline xi::Record& append(xi::Record& rec, const std::string& key, const char* v, int line)
    { rec.set(key, std::string(v)); detail::layout_push(rec, key, "value", line); return rec; }
inline xi::Record& append(xi::Record& rec, const std::string& key, const xi::Image& im, int line)
    { rec.image(key, im);           detail::layout_push(rec, key, "image", line); return rec; }

// VAR-style sugar over append(); stamps the call's source line so the UI can show
// "this value came from line N" (teach / debug).
#define pvar(rec, key, data) ::xi::preview::append((rec), (key), (data), __LINE__)

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
