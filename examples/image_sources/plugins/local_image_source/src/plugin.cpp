//
// local_image_source.cpp — a webui image source.
//
// Scans a local folder for images, shows them as a clickable thumbnail grid in
// its instance UI, and on a click ISSUES the chosen image as a trigger (mints a
// fresh trigger id via emit_trigger → the pipeline runs one pass on that image).
// This is the "pick a local file and run it" half of the HDevelop-style dev loop
// (the cached_image_source plugin is the replay-from-history half).
//
// Design notes (see docs/guides/cache-and-sources.md):
//   - The EMITTER owns the trigger id: we pass XI_TRIGGER_NULL so the bus mints a
//     fresh, collision-free id per issue. The file's own identity (its filename)
//     is the source's key/provenance, kept separate from the dispatch id.
//   - The image rides ON the trigger (emit_trigger carries it); the script reads
//     it back via xi::current_trigger().image("<this instance's name>").
//
// exchange() commands (from ui/index.html):
//   "get_status"            -> { dir, files:[{name,w,h,thumb}], issued_name, ... }
//   "set_dir" {value}       -> set the folder + rescan
//   "refresh"               -> rescan the folder
//   "issue" {index}         -> load file[index] full-res and emit it as a trigger
//

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_image.hpp>     // xi::Image (+ as_cv_mat). JPEG via cv::imencode
                               // (OpenCV is linked; xi::encode_jpeg's stb fallback
                               // would pull an unresolved stbi_write symbol here).

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
// Minimal base64 for embedding a JPEG thumbnail in the exchange JSON as a
// data: URI the webview <img> can render directly.
std::string base64(const std::vector<uint8_t>& d) {
    static const char* a =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        unsigned n = d[i] << 16;
        if (i + 1 < d.size()) n |= d[i + 1] << 8;
        if (i + 2 < d.size()) n |= d[i + 2];
        o.push_back(a[(n >> 18) & 63]);
        o.push_back(a[(n >> 12) & 63]);
        o.push_back(i + 1 < d.size() ? a[(n >> 6) & 63] : '=');
        o.push_back(i + 2 < d.size() ? a[n & 63] : '=');
    }
    return o;
}

bool is_image_ext(const std::string& e) {
    std::string s; s.reserve(e.size());
    for (char c : e) s.push_back((char)std::tolower((unsigned char)c));
    return s == ".png" || s == ".jpg" || s == ".jpeg" || s == ".bmp" || s == ".tif" || s == ".tiff";
}
} // namespace

class LocalImageSource : public xi::Plugin {
public:
    using xi::Plugin::Plugin;

    // Headless/script path: process({"index":N}) loads & returns the image too,
    // so the source is usable without the UI (e.g. in a driver test).
    xi::Record process(const xi::Record& input) override {
        scan_();
        int idx = input["index"].as_int(0);
        xi::Image img = load_(idx);
        if (img.empty())
            return xi::Record().set("loaded", false).set("index", idx);
        return xi::Record().image("frame", img).set("loaded", true)
            .set("index", idx).set("width", img.width).set("height", img.height);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "set_dir") {
            dir_ = p["value"].as_string(dir_);
            scanned_ = false;
        } else if (command == "refresh") {
            scanned_ = false;
        } else if (command == "issue") {
            scan_();
            int idx = p["index"].as_int(-1);
            issue_(idx);
        }
        return status_();   // every command returns the fresh status (incl. thumbs)
    }

    std::string get_def() const override { return const_cast<LocalImageSource*>(this)->status_(); }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        dir_ = p["dir"].as_string(dir_);
        scanned_ = false;
        return true;
    }

private:
    std::string              dir_;
    bool                     scanned_ = false;
    std::vector<std::string> files_;          // absolute paths, sorted
    std::string              issued_name_;
    int                      issued_count_ = 0;

    void scan_() {
        if (scanned_) return;
        files_.clear();
        std::error_code ec;
        if (!dir_.empty() && fs::is_directory(dir_, ec)) {
            for (auto& e : fs::directory_iterator(dir_, ec)) {
                if (ec) break;
                if (e.is_regular_file(ec) && is_image_ext(e.path().extension().string()))
                    files_.push_back(e.path().string());
            }
            std::sort(files_.begin(), files_.end());
        }
        scanned_ = true;
    }

    // Load file[idx] full-resolution into an xi::Image (via the host's image
    // reader → a pool handle we copy out of and release).
    xi::Image load_(int idx) {
        if (idx < 0 || idx >= (int)files_.size() || !host()) return {};
        xi_image_handle h = host()->read_image_file(files_[(size_t)idx].c_str());
        if (!h) return {};
        xi::Image img = xi::Image::adopt_pool_handle(host(), h);  // addref's
        host()->image_release(h);                                  // drop the read ref
        return img;   // owns its own copy via adopt
    }

    // Issue: emit file[idx] as a fresh-id trigger carrying the image.
    void issue_(int idx) {
        if (idx < 0 || idx >= (int)files_.size() || !host()) return;
        xi::Image img = load_(idx);
        if (img.empty()) return;
        // Copy into a pool handle to attach to the trigger (burst_source pattern).
        xi_image_handle h = host()->image_create(img.width, img.height, img.channels);
        if (!h) return;
        std::memcpy(host()->image_data(h), img.data(), img.size());
        xi_record_image rec{ "frame", h };
        host()->emit_trigger(name().c_str(), XI_TRIGGER_NULL, /*ts=*/0, &rec, 1);
        host()->image_release(h);   // bus addref'd; drop our ref
        issued_name_ = fs::path(files_[(size_t)idx]).filename().string();
        ++issued_count_;
    }

    // Build the status JSON the webui renders: config + a thumbnail per file.
    std::string status_() {
        scan_();
        auto files = xi::Json::array();
        for (auto& path : files_) {
            auto entry = xi::Json::object();
            entry.set("name", fs::path(path).filename().string());
            std::string thumb; int w = 0, h = 0;
            make_thumb_(path, thumb, w, h);
            entry.set("w", w).set("h", h).set("thumb", thumb);
            files.push(entry);
        }
        return xi::Json::object()
            .set("dir", dir_)
            .set("count", (int)files_.size())
            .set("issued_name", issued_name_)
            .set("issued_count", issued_count_)
            .set("files", files)
            .dump();
    }

    // Small JPEG thumbnail (~140px wide) as a data: URI. Best-effort: empty on fail.
    void make_thumb_(const std::string& path, std::string& out_uri, int& w, int& h) {
        out_uri.clear(); w = h = 0;
        if (!host()) return;
        xi_image_handle hd = host()->read_image_file(path.c_str());
        if (!hd) return;
        xi::Image img = xi::Image::adopt_pool_handle(host(), hd);
        host()->image_release(hd);
        if (img.empty()) return;
        w = img.width; h = img.height;
        const int TW = 140;
        cv::Mat src = img.as_cv_mat();   // view into img (alive for this scope)
        cv::Mat small;
        if (img.width > TW) {
            int th = std::max(1, img.height * TW / img.width);
            cv::resize(src, small, cv::Size(TW, th), 0, 0, cv::INTER_AREA);
        } else {
            src.copyTo(small);
        }
        // xi::Image is RGB; cv encodes BGR — swap 3-channel so colours are right.
        cv::Mat bgr;
        if (small.channels() == 3) cv::cvtColor(small, bgr, cv::COLOR_RGB2BGR);
        else                       bgr = small;
        std::vector<uint8_t> jpeg;
        std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 60 };
        if (cv::imencode(".jpg", bgr, jpeg, params))
            out_uri = "data:image/jpeg;base64," + base64(jpeg);
    }
};

XI_PLUGIN_IMPL(LocalImageSource)
