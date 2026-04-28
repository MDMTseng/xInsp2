//
// blob_analysis.cpp — threshold + contour analysis plugin.
//
// Input Record:
//   image "gray"           — single-channel grayscale image
//   "threshold"   (int)    — threshold value (default 128)
//   "min_area"    (int)    — minimum blob area to keep (default 10)
//   "max_area"    (int)    — maximum blob area (default 999999)
//   "invert"      (bool)   — invert threshold (default false)
//
// Output Record:
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

#include <xi/xi_abi.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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

class BlobAnalysis : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    xi::Record process(const xi::Record& input) override {
        auto gray = input.get_image("gray");
        if (gray.empty() || gray.channels != 1) {
            return xi::Record().set("error", "input 'gray' must be a single-channel image");
        }

        int thresh    = input["threshold"].as_int(thresh_);
        int min_area  = input["min_area"].as_int(min_area_);
        int max_area  = input["max_area"].as_int(max_area_);
        bool inv      = input["invert"].as_bool(invert_);

        int w = gray.width, h = gray.height;

        // Threshold
        xi::Image binary(w, h, 1);
        const uint8_t* sp = gray.data();
        uint8_t* dp = binary.data();
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
        out.image("binary", std::move(binary));
        out.set("blob_count", (int)blobs.size());
        out.set("threshold_used", thresh);

        for (auto& b : blobs) {
            xi::Record blob_rec;
            blob_rec.set("area", b.area);
            blob_rec.set("cx", b.cx);
            blob_rec.set("cy", b.cy);
            blob_rec.set("min_x", b.min_x);
            blob_rec.set("min_y", b.min_y);
            blob_rec.set("max_x", b.max_x);
            blob_rec.set("max_y", b.max_y);
            blob_rec.set("contour_points", (int)b.contour.size());

            // Store contour as array of {x,y} objects
            for (auto& [px, py] : b.contour) {
                blob_rec.push("contour", xi::Record().set("x", px).set("y", py));
            }

            out.push("blobs", std::move(blob_rec));
        }

        cache_result(out);
        return out;
    }

    std::string exchange(const std::string& cmd) override {
        if (cmd.find("\"set_threshold\"") != std::string::npos) {
            auto pos = cmd.find("\"value\":");
            if (pos != std::string::npos) thresh_ = std::stoi(cmd.substr(pos + 8));
        }
        if (cmd.find("\"set_min_area\"") != std::string::npos) {
            auto pos = cmd.find("\"value\":");
            if (pos != std::string::npos) min_area_ = std::stoi(cmd.substr(pos + 8));
        }
        if (cmd.find("\"set_invert\"") != std::string::npos) {
            invert_ = cmd.find("\"value\":true") != std::string::npos;
        }
        if (cmd.find("\"get_last_result\"") != std::string::npos && last_result_json_.size() > 2) {
            return last_result_json_;
        }
        return get_def();
    }

    // Cache last process result so the UI can fetch it via exchange
    void cache_result(const xi::Record& r) {
        last_result_json_ = r.data_json();
    }

    std::string get_def() const override {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            R"({"threshold":%d,"min_area":%d,"max_area":%d,"invert":%s})",
            thresh_, min_area_, max_area_, invert_ ? "true" : "false");
        return buf;
    }

    bool set_def(const std::string& json) override {
        xi::Record r;
        cJSON* p = cJSON_Parse(json.c_str());
        if (!p) return false;
        cJSON* t = cJSON_GetObjectItem(p, "threshold"); if (t) thresh_ = t->valueint;
        cJSON* a = cJSON_GetObjectItem(p, "min_area");  if (a) min_area_ = a->valueint;
        cJSON* m = cJSON_GetObjectItem(p, "max_area");  if (m) max_area_ = m->valueint;
        cJSON* i = cJSON_GetObjectItem(p, "invert");    if (i) invert_ = cJSON_IsTrue(i);
        cJSON_Delete(p);
        return true;
    }

private:
    int thresh_ = 128;
    int min_area_ = 10;
    int max_area_ = 999999;
    bool invert_ = false;
    std::string last_result_json_;
};

XI_PLUGIN_IMPL(BlobAnalysis)
