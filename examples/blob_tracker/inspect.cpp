//
// inspect.cpp — blob tracker. Counts left-to-right gate crossings.
//
// Drives ONE plugin ("det" / blob_centroid_detector) which produces a
// list of {x,y,area} centroids per frame. Tracking — matching this
// frame's centroids to the previous frame's centroids and detecting
// gate crossings — lives here in the script because it's
// per-script semantics, not a reusable image-math op.
//
// Cross-frame memory is held in xi::state(). On first call the
// state Record is empty, so the script treats every centroid as a
// "new" blob (no previous match → no crossing test). From the second
// call on, prev_centroids is the array we wrote at the end of the
// previous call.
// State schema version — INTENT.
//
// The intent is: if I later change the shape of `prev_centroids`
// (e.g. add an `id` field), the next compile_and_load fires
// `event:state_dropped` and the new script starts with empty state
// instead of silently default-filling and miscounting.
//
// Reality (2026-04-28): the documented override path is broken
// for scripts. xi_script_support.hpp is force-included via cl.exe
// /FI, which preprocesses it BEFORE this TU. By the time
// the `#define XI_STATE_SCHEMA_VERSION 1` below is seen, the thunk
// `xi_script_state_schema_version()` has already been generated
// returning 0. The redefinition produces a benign C4005 warning
// and changes nothing observable. See RESULTS.md → friction notes.
//
// Keeping the define here as documentation of intent + a forward-
// compatibility stamp once the FI ordering is fixed. Until then,
// this script reports schema version 0 to the host.
#undef  XI_STATE_SCHEMA_VERSION
#define XI_STATE_SCHEMA_VERSION 1

#include <xi/xi.hpp>
#include <xi/xi_use.hpp>

#include <cmath>
#include <vector>

// ---- Tunables exposed in the script-params strip --------------------

// Vertical gate line; crossing detection fires when prev.x < gate_x and
// cur.x >= gate_x. Default 320 = middle of the 640-px frame.
static xi::Param<int> gate_x{"gate_x", 320, {0, 4096}};

// Maximum L2 distance (px) between a centroid in this frame and a
// centroid in the previous frame for them to be considered the
// SAME blob. Worst per-frame motion in this dataset is 20 px, so 60
// is a comfortable margin — large enough to bridge a single
// detection miss, small enough that we don't accidentally pair
// blobs in different y-rows (closest pair of blob ys is 100 vs 200,
// i.e. 100 px apart).
static xi::Param<int> match_max_dist{"match_max_dist", 60, {1, 400}};

// ---- helpers --------------------------------------------------------

namespace {

struct Centroid { int x, y, area; };

// Build a plain JSON array of {x,y,area} from the detector's output.
// `centroids_node` is record["centroids"] — a Record::Value pointing at
// a cJSON array (or nothing, on error).
std::vector<Centroid> read_centroid_list(const xi::Record::Value& v) {
    std::vector<Centroid> out;
    int n = v.size();
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        Centroid c{
            v[i]["x"].as_int(0),
            v[i]["y"].as_int(0),
            v[i]["area"].as_int(0),
        };
        out.push_back(c);
    }
    return out;
}

// Same shape, reading from xi::state()["prev_centroids"].
std::vector<Centroid> read_state_centroids(const xi::Record::Value& v) {
    return read_centroid_list(v);
}

// Build a cJSON array of {x,y,area} from a centroid vector — used for
// stuffing into xi::state() via set_raw().
cJSON* build_centroid_array(const std::vector<Centroid>& cs) {
    cJSON* arr = cJSON_CreateArray();
    for (auto& c : cs) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "x",    c.x);
        cJSON_AddNumberToObject(o, "y",    c.y);
        cJSON_AddNumberToObject(o, "area", c.area);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

double dist2(const Centroid& a, const Centroid& b) {
    double dx = a.x - b.x, dy = a.y - b.y;
    return dx*dx + dy*dy;
}

} // namespace

// ---- entry ----------------------------------------------------------

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    // 1) Read tunables once at the top.
    int GATE = gate_x;
    int MAX_DIST = match_max_dist;
    double max_d2 = (double)MAX_DIST * (double)MAX_DIST;

    // 2) Load the frame the host pointed us at.
    std::string fpath = xi::current_frame_path();
    xi::Image frame = xi::imread(fpath);
    if (frame.empty()) {
        VAR(error,      std::string("imread failed: ") + fpath);
        VAR(frame_path, fpath);
        return;
    }
    VAR(input,      frame);
    VAR(frame_path, fpath);

    // 3) Detector → centroid list (+ diagnostic images).
    auto& det = xi::use("det");
    auto det_out = det.process(xi::Record().image("src", frame));
    if (det_out.has("error")) {
        VAR(error, det_out["error"].as_string("detector error"));
        return;
    }
    VAR(mask,           det_out.get_image("mask"));
    VAR(cleaned,        det_out.get_image("cleaned"));
    VAR(n_blobs,        det_out["count"].as_int(0));
    VAR(rejected_small, det_out["rejected_small"].as_int(0));
    VAR(rejected_big,   det_out["rejected_big"].as_int(0));

    auto cur = read_centroid_list(det_out["centroids"]);

    // 4) Pull prior state. (Locals shadow the VAR names we'll emit
    //    later — VAR(name, expr) declares `auto name = expr;` so the
    //    locals here must use distinct identifiers, hence the `_v` suffix.)
    int  frame_seq_v        = xi::state()["frame_seq"].as_int(0);
    int  crossings_so_far_v = xi::state()["crossings_so_far"].as_int(0);
    auto prev               = read_state_centroids(xi::state()["prev_centroids"]);

    // 5) Tracking — greedy NN. For each current centroid, find the
    //    nearest unmatched prev centroid; if within MAX_DIST, that's
    //    the same blob. Test for gate crossing.
    int delta_crossings_v = 0;
    int matched_v         = 0;
    std::vector<bool> prev_taken(prev.size(), false);
    for (auto& c : cur) {
        int    best_i = -1;
        double best_d2 = max_d2;  // strict <=; we only accept if within
        for (size_t i = 0; i < prev.size(); ++i) {
            if (prev_taken[i]) continue;
            double d2 = dist2(c, prev[i]);
            if (d2 <= best_d2) {
                best_d2 = d2;
                best_i  = (int)i;
            }
        }
        if (best_i >= 0) {
            prev_taken[best_i] = true;
            ++matched_v;
            // Gate crossing: prev was on/left of gate and cur is on/right.
            if (prev[best_i].x < GATE && c.x >= GATE) {
                ++delta_crossings_v;
            }
        }
        // else: blob just appeared this frame (entered from edge or
        // detector missed it last frame). Either way, we skip the
        // crossing test since we have no anchor for this blob's prior x.
    }
    crossings_so_far_v += delta_crossings_v;

    // 6) Persist state for the next call. set_raw takes ownership of
    //    the cJSON node we pass it.
    xi::state().set("frame_seq",          frame_seq_v + 1);
    xi::state().set("crossings_so_far",   crossings_so_far_v);
    xi::state().set_raw("prev_centroids", build_centroid_array(cur));

    // 7) Emit per-frame VARs the driver can read. VAR(name, expr)
    //    expands to `auto name = expr;` — so we name them differently
    //    from the locals above.
    VAR(frame_seq,         frame_seq_v);          // frame index this CALL processed
    VAR(crossings_so_far,  crossings_so_far_v);   // running total after this frame
    VAR(delta_crossings,   delta_crossings_v);    // crossings detected this frame
    VAR(matched_blobs,     matched_v);            // blobs matched to prev frame
    VAR(prev_blob_count,   (int)prev.size());     // size of state we read in
    VAR(gate_x_used,       GATE);
}
