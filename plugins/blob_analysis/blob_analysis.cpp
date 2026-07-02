//
// blob_analysis.cpp — threshold + contour analysis plugin.
//
// Input/output keys are named ONCE in blob_analysis_keys.h and consumed with a
// typed view (blob_analysis_io.h); this reader compiles from the same constants
// so it can't drift from the wrapper a script includes. See
// docs/new_gen/02-plugin-data-contract.md for the contract this implements.
//
// Input Record  (build it with xi::blob::Input):
//   image "gray"           — single-channel grayscale image (REQUIRED)
//   "threshold"   (int)    — threshold value (default 128)
//   "min_area"    (int)    — minimum blob area to keep (default 10)
//   "max_area"    (int)    — maximum blob area (default 999999)
//   "invert"      (bool)   — invert threshold (default false)
//
// Output Record  (read it with xi::blob::Output):
//   image "binary"         — thresholded binary image
//   "blob_count"  (int)    — number of blobs found
//   "blobs"       (array)  — per-blob data:
//     [i].area     (int)
//     [i].cx       (double)  — centroid x
//     [i].cy       (double)  — centroid y
//     [i].min_x    (int)     — bounding box
//     [i].min_y    (int)
//     [i].max_x    (int)
//     [i].max_y    (int)
//     [i].contour  (array)   — [{x,y}, ...] contour points
//
// A missing "gray" input (or a header/plugin schema skew) is returned as a
// structured NA failure the script maps to a verdict — see xi::contract.
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>       // parses exchange/def commands (canonical over cmd.find)
#include <xi/xi_contract.hpp>   // fail-loud required inputs + schema-skew errors
#include <xi/xi_mp.hpp>         // wave-2: canonical msgpack for the frame-door contour (doc 07 D3)

#include "blob_analysis_keys.gen.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

struct BlobInfo {
    int area = 0;
    double cx = 0, cy = 0;
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    std::vector<std::pair<int,int>> contour;
};

// Flood-fill labeling + contour tracing
static std::vector<BlobInfo> find_blobs(const uint8_t* bin, int w, int h,
                                         int min_area, int max_area) {
    std::vector<int> labels(w * h, 0);
    std::vector<BlobInfo> blobs;
    int next_label = 0;

    std::vector<std::pair<int,int>> stack;
    stack.reserve(1024);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (bin[y * w + x] == 0 || labels[y * w + x] != 0) continue;
            ++next_label;

            BlobInfo blob;
            blob.min_x = x; blob.max_x = x;
            blob.min_y = y; blob.max_y = y;
            int64_t sum_x = 0, sum_y = 0;

            stack.clear();
            stack.push_back({x, y});

            while (!stack.empty()) {
                auto [cx, cy] = stack.back();
                stack.pop_back();
                if (cx < 0 || cx >= w || cy < 0 || cy >= h) continue;
                if (bin[cy * w + cx] == 0 || labels[cy * w + cx] != 0) continue;
                labels[cy * w + cx] = next_label;
                blob.area++;
                sum_x += cx;
                sum_y += cy;
                blob.min_x = std::min(blob.min_x, cx);
                blob.max_x = std::max(blob.max_x, cx);
                blob.min_y = std::min(blob.min_y, cy);
                blob.max_y = std::max(blob.max_y, cy);

                stack.push_back({cx - 1, cy});
                stack.push_back({cx + 1, cy});
                stack.push_back({cx, cy - 1});
                stack.push_back({cx, cy + 1});
            }

            if (blob.area >= min_area && blob.area <= max_area) {
                blob.cx = (double)sum_x / blob.area;
                blob.cy = (double)sum_y / blob.area;

                // Extract contour: boundary pixels (have at least one
                // non-blob 4-neighbor)
                for (int by = blob.min_y; by <= blob.max_y; ++by) {
                    for (int bx = blob.min_x; bx <= blob.max_x; ++bx) {
                        if (labels[by * w + bx] != next_label) continue;
                        bool is_border = false;
                        if (bx == 0 || bx == w-1 || by == 0 || by == h-1) is_border = true;
                        else {
                            if (labels[(by-1)*w+bx] != next_label ||
                                labels[(by+1)*w+bx] != next_label ||
                                labels[by*w+(bx-1)] != next_label ||
                                labels[by*w+(bx+1)] != next_label)
                                is_border = true;
                        }
                        if (is_border) blob.contour.push_back({bx, by});
                    }
                }
                blobs.push_back(std::move(blob));
            }
        }
    }
    return blobs;
}

namespace keys = xi::blob::keys;

class BlobAnalysis : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        // Guard 3: reject a script compiled against an incompatible header
        // version with a precise error that names both versions.
        if (auto skew = xi::contract::check_schema(input, xi::blob::kSchemaVersion))
            return *skew;

        // Guard 2: a missing/mis-typed required input is a structured, fail-loud
        // failure the script maps to a verdict — never a silent default.
        if (auto miss = xi::contract::require_image(input, keys::kGray, 1))
            return *miss;

        // Schemaless tolerance retained: unknown extra keys are ignored, warned once.
        xi::contract::warn_unknown_keys(
            input,
            {keys::kGray, keys::kThreshold, keys::kMinArea, keys::kMaxArea, keys::kInvert},
            warned_unknown_, [this](const std::string& m) { log_warn(m); });

        const xi::Image& gray = input.get_image(keys::kGray);

        // Snapshot config defaults under the lock (exchange()/set_def() write them
        // from another thread); per-frame input keys override each default.
        int def_thresh, def_min, def_max; bool def_inv;
        {
            std::lock_guard<std::mutex> lk(mu_);
            def_thresh = thresh_; def_min = min_area_; def_max = max_area_; def_inv = invert_;
        }
        int thresh   = input[keys::kThreshold].as_int(def_thresh);
        int min_area = input[keys::kMinArea].as_int(def_min);
        int max_area = input[keys::kMaxArea].as_int(def_max);
        bool inv     = input[keys::kInvert].as_bool(def_inv);

        int w = gray.width, h = gray.height;

        // Threshold — write straight into a pool slot so the cross-ABI return
        // path is a zero-copy addref, not a heap-to-pool memcpy.
        xi::Image binary = pool_image(w, h, 1);
        const uint8_t* sp = gray.read();
        uint8_t* dp = binary.write();
        if (!dp) return xi::Record::na("blob_analysis: could not allocate output image");
        for (int i = 0; i < w * h; ++i) {
            if (inv)
                dp[i] = (sp[i] < thresh) ? 255 : 0;
            else
                dp[i] = (sp[i] > thresh) ? 255 : 0;
        }

        // Find blobs
        auto blobs = find_blobs(dp, w, h, min_area, max_area);

        // Build output
        xi::Record out;
        out.image(keys::kBinary, std::move(binary));
        out.set(keys::kBlobCount, (int)blobs.size());
        out.set(keys::kThresholdUsed, thresh);

        for (auto& b : blobs) {
            xi::Record blob_rec;
            blob_rec.set(keys::kArea, b.area);
            blob_rec.set(keys::kCx, b.cx);
            blob_rec.set(keys::kCy, b.cy);
            blob_rec.set(keys::kMinX, b.min_x);
            blob_rec.set(keys::kMinY, b.min_y);
            blob_rec.set(keys::kMaxX, b.max_x);
            blob_rec.set(keys::kMaxY, b.max_y);
            blob_rec.set(keys::kContourPoints, (int)b.contour.size());

            // Store contour as array of {x,y} objects
            for (auto& [px, py] : b.contour) {
                blob_rec.push(keys::kContour, xi::Record().set(keys::kX, px).set(keys::kY, py));
            }

            out.push(keys::kBlobs, std::move(blob_rec));
        }

        cache_result(out);
        return out;
    }

    // polaris2 wave-2 (docs/new_gen/08 Wave 2): the xi.frame@1 frame-in/frame-out
    // door. Consumes the SAME contract keyset as the Record process() above (the
    // blob_analysis_keys.h constants ARE the frame schema — read via FrameIn's
    // typed accessors, no string literals at the call sites) and produces its
    // outputs as a frame: the binary image entry, the scalar counts, and the
    // blobs array (incl. each blob's contour polygon) as ONE nested canonical-
    // msgpack entry (doc 07 D3 — nesting is msgpack's job). Fail-loud carries
    // over: a missing/mis-typed 'gray' is a normal sealed frame stamped with the
    // xi::contract reason code (FrameOut::fault), which the caller routes to a
    // verdict — never a silent default. The Record process() above is untouched;
    // this instance speaks BOTH currencies.
    void process_frame(xi::FrameIn& in, xi::FrameOut& out) override {
        auto gray = in.image(keys::kGray);
        if (!gray || !gray->pixels) {
            out.fault(xi::contract::kMissingInput, keys::kGray,
                      "blob_analysis(frame): required image 'gray' is missing");
            return;
        }
        if (gray->channels != 1) {
            out.fault(xi::contract::kWrongType, keys::kGray,
                      "blob_analysis(frame): 'gray' must be single-channel");
            return;
        }
        const int w = gray->width, h = gray->height;
        const uint8_t* sp = static_cast<const uint8_t*>(gray->pixels);

        int def_thresh, def_min, def_max; bool def_inv;
        {
            std::lock_guard<std::mutex> lk(mu_);
            def_thresh = thresh_; def_min = min_area_; def_max = max_area_; def_inv = invert_;
        }
        const int  thresh   = (int)in.i64_or(keys::kThreshold, def_thresh);
        const int  min_area = (int)in.i64_or(keys::kMinArea,   def_min);
        const int  max_area = (int)in.i64_or(keys::kMaxArea,   def_max);
        const bool inv      = in.bool_or(keys::kInvert, def_inv);

        std::vector<uint8_t> bin((size_t)w * (size_t)h);
        for (int i = 0; i < w * h; ++i)
            bin[i] = inv ? (sp[i] < thresh ? 255 : 0) : (sp[i] > thresh ? 255 : 0);

        auto blobs = find_blobs(bin.data(), w, h, min_area, max_area);

        out.image(keys::kBinary, w, h, 1, bin.data());
        out.i64(keys::kBlobCount, (int64_t)blobs.size());
        out.i64(keys::kThresholdUsed, thresh);

        // The blobs array (incl. each contour) as one nested canonical-msgpack
        // entry — the leaky-veneer contour the _io.h struggled with (synthesis §4)
        // rides naturally here: msgpack maps/arrays nest without a flattened key
        // convention.
        xi::mp::Writer mw;
        mw.array((uint32_t)blobs.size());
        for (auto& b : blobs) {
            mw.map(9);
            mw.key(keys::kArea);          mw.int_(b.area);
            mw.key(keys::kCx);            mw.float_(b.cx);
            mw.key(keys::kCy);            mw.float_(b.cy);
            mw.key(keys::kMinX);          mw.int_(b.min_x);
            mw.key(keys::kMinY);          mw.int_(b.min_y);
            mw.key(keys::kMaxX);          mw.int_(b.max_x);
            mw.key(keys::kMaxY);          mw.int_(b.max_y);
            mw.key(keys::kContourPoints); mw.int_((int64_t)b.contour.size());
            mw.key(keys::kContour);       mw.array((uint32_t)b.contour.size());
            for (auto& [px, py] : b.contour) {
                mw.map(2);
                mw.key(keys::kX); mw.int_(px);
                mw.key(keys::kY); mw.int_(py);
            }
        }
        out.mp(keys::kBlobs, mw.bytes().data(), mw.bytes().size());
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string command = p["command"].as_string();

        std::lock_guard<std::mutex> lk(mu_);
        if      (command == "set_threshold") thresh_   = p["value"].as_int(thresh_);
        else if (command == "set_min_area")  min_area_ = p["value"].as_int(min_area_);
        else if (command == "set_invert")    invert_   = p["value"].as_bool(invert_);
        else if (command == "get_last_result" && last_result_json_.size() > 2)
            return last_result_json_;
        return get_def_locked_();
    }

    // Cache last process result so the UI can fetch it via exchange.
    void cache_result(const xi::Record& r) {
        std::lock_guard<std::mutex> lk(mu_);
        last_result_json_ = r.data_json();
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return get_def_locked_();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        thresh_   = p[keys::kThreshold].as_int(thresh_);
        min_area_ = p[keys::kMinArea].as_int(min_area_);
        max_area_ = p[keys::kMaxArea].as_int(max_area_);
        invert_   = p[keys::kInvert].as_bool(invert_);
        return true;
    }

private:
    // Serialize config to JSON — caller holds mu_.
    std::string get_def_locked_() const {
        return xi::Json::object()
            .set(keys::kThreshold, thresh_)
            .set(keys::kMinArea, min_area_)
            .set(keys::kMaxArea, max_area_)
            .set(keys::kInvert, invert_)
            .dump();
    }

    mutable std::mutex mu_;   // guards config members + last_result_json_
    int thresh_ = 128;
    int min_area_ = 10;
    int max_area_ = 999999;
    bool invert_ = false;
    bool warned_unknown_ = false;
    std::string last_result_json_;
};

XI_PLUGIN_IMPL(BlobAnalysis)
// polaris2 wave-2: publish the xi.frame@1 frame-in/frame-out door (the plugin-
// side capability door — the synthesis §3 pure-door dry run). The host probes
// xi_plugin_get_interface("xi.frame", 1) to learn blob speaks frames.
XI_PLUGIN_FRAME_DOOR(BlobAnalysis)
