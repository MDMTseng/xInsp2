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
//   - The image rides ON the trigger (a pack carries it under key "frame");
//     the script reads it back via t.pack().get_image("frame") — the same key
//     cmd:run/replay inject under, so one script works against both.
//
// exchange() commands (from ui/index.html):
//   "get_status"            -> { dir, files:[{name,w,h,thumb}], issued_name, ... }
//   "set_dir" {value}       -> set the folder + rescan
//   "refresh"               -> rescan the folder
//   "issue" {index}         -> load file[index] full-res and emit it as a trigger
//

#include <xi/xi_abi.hpp>
#include <xi/xi_cv.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_image.hpp>     // xi::Image (+ as_cv_mat). JPEG via cv::imencode
                               // (OpenCV is linked; xi::encode_jpeg's stb fallback
                               // would pull an unresolved stbi_write symbol here).

#include <algorithm>
#include <atomic>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
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
    ~LocalImageSource() override { stop_auto_(); }

    // Headless/script path (v12 pack door): process({"index":N}) loads the
    // chosen image and returns it as a pack keyed "frame", so the source is
    // usable without the UI (e.g. in a driver test).
    void process(xi::PackIn& in, xi::PackOut& out) override {
        std::lock_guard<std::mutex> lk(mu_);
        scan_();
        int idx = (int)in.i64_or("index", 0);
        xi::Image img = load_(idx);
        if (img.empty()) {
            out.boolean("loaded", false).i64("index", idx);
            return;
        }
        out.image("frame", img.width, img.height, img.channels, img.read())
           .boolean("loaded", true).i64("index", idx)
           .i64("width", img.width).i64("height", img.height);
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        std::lock_guard<std::mutex> lk(mu_);
        if (command == "set_dir") {
            dir_ = p["value"].as_string(dir_);
            scanned_ = false;
        } else if (command == "refresh") {
            scanned_ = false;
        } else if (command == "issue") {
            scan_();
            int idx = p["index"].as_int(-1);
            issue_(idx);
        } else if (command == "auto_on") {
            apply_auto_ms_(p["value"].as_int(auto_ms_.load() > 0 ? auto_ms_.load() : 500));
        } else if (command == "auto_off") {
            apply_auto_ms_(0);
        }
        return status_();   // every command returns the fresh status (incl. thumbs)
    }

    std::string get_def() const override {
        auto* self = const_cast<LocalImageSource*>(this);
        std::lock_guard<std::mutex> lk(self->mu_);
        return self->status_();
    }

    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        dir_ = p["dir"].as_string(dir_);
        scanned_ = false;
        // auto_ms > 0 → self-emit the folder's images on a timer (auto-update).
        apply_auto_ms_(p["auto_ms"].as_int(auto_ms_.load()));
        return true;
    }

private:
    std::mutex               mu_;
    const xi_cap_v1*  cap_ = nullptr;           // xi.cap funnel (resolved once)
    std::atomic<bool> cap_resolved_{false};            // guards dir_/files_/scanned_/issued_*
    std::string              dir_;
    bool                     scanned_ = false;
    std::vector<std::string> files_;          // absolute paths, sorted
    std::string              issued_name_;
    int                      issued_count_ = 0;
    // Auto self-emit (auto_ms > 0): a worker re-scans the folder + emits the next
    // image every auto_ms, cycling — so dropping/replacing a file auto-updates.
    std::atomic<int>         auto_ms_{0};
    std::atomic<bool>        auto_run_{false};
    std::thread              auto_thread_;
    size_t                   auto_idx_ = 0;

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

    // v12 THE CUT: decode a file via the xi.image.decode CAPABILITY (imgcodec —
    // this project declares a "codec" instance; the v11 host decode slot is
    // gone). Reads the bytes, calls the funnel (thread-safe: handlers accept
    // concurrent calls, so the auto worker may use this too), copies the reply
    // image out. Empty Image when the provider is absent / the file won't decode.
    xi::Image decode_file_(const std::string& path) {
        const xi_pack_v1* pk = pack_iface();
        if (!pk || !host() || !host()->get_interface) return {};
        if (!cap_resolved_.exchange(true)) {
            cap_ = static_cast<const xi_cap_v1*>(host()->get_interface("xi.cap", 1));
        }
        const xi_cap_v1* cap = cap_;
        if (!cap) return {};
        std::ifstream f(path, std::ios::binary);
        if (!f) return {};
        std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
        if (bytes.empty()) return {};
        xi_pack_builder b = pk->builder_new();
        pk->builder_add_bin(b, "data", bytes.data(), (int32_t)bytes.size());
        pk->builder_add_i64(b, "$v", 1);
        xi_pack_handle req = pk->builder_seal(b);
        xi_pack_handle rsp = XI_PACK_NULL;
        const int32_t rc = cap->call("xi.image.decode", req, &rsp);
        pk->release(req);
        xi::Image img;
        if (rc == XI_CAP_OK && rsp != XI_PACK_NULL) {
            xi_pack_image di{};
            if (pk->get_image(rsp, "image", &di) && di.pixels && di.width > 0)
                img = xi::Image(di.width, di.height, di.channels,
                                static_cast<const uint8_t*>(di.pixels));  // copies
            pk->release(rsp);
        }
        return img;
    }

    // Load file[idx] full-resolution into an xi::Image (capability decode).
    xi::Image load_(int idx) {
        if (idx < 0 || idx >= (int)files_.size() || !host()) return {};
        return decode_file_(files_[(size_t)idx]);
    }

    // Load `path` full-res FROM DISK and emit it as a fresh-id trigger. Touches no
    // shared members except host() (thread-safe), so it's safe to call WITHOUT mu_
    // (the auto worker does). Reading the file each time means a replaced/edited
    // file is auto-reloaded on its next emit.
    bool emit_file_(const std::string& path) {
        if (!host()) return false;
        xi::Image img = decode_file_(path);
        if (img.empty()) return false;
        // Emit the loaded image as a fresh-id trigger, PACK-plane (polaris2 THE
        // CUT): build a pack carrying the frame under key "frame" and emit it —
        // the script reads it back via t.pack().get_image("frame").
        xi::PackOut out = new_pack();
        if (!out.valid()) return false;            // host has no pack plane
        out.image("frame", img.width, img.height, img.channels, img.read());
        emit(std::move(out), XI_TRIGGER_NULL);
        return true;
    }

    // Manual issue (UI click). Caller holds mu_.
    void issue_(int idx) {
        if (idx < 0 || idx >= (int)files_.size()) return;
        if (emit_file_(files_[(size_t)idx])) {
            issued_name_ = fs::path(files_[(size_t)idx]).filename().string();
            ++issued_count_;
        }
    }

    // Turn auto self-emit on (ms>0) / off (ms==0). Caller holds mu_. The worker is
    // long-lived (spawned on first activation, joined in the dtor); ms==0 just
    // parks it idle, so toggling never has to join under the lock.
    void apply_auto_ms_(int ms) {
        if (ms < 0) ms = 0;
        auto_ms_.store(ms);
        if (ms > 0 && !auto_run_.exchange(true))
            auto_thread_ = std::thread([this] { run_auto_(); });
    }
    void stop_auto_() {
        auto_run_.store(false);
        if (auto_thread_.joinable()) auto_thread_.join();
    }
    void run_auto_() {
        using clk = std::chrono::steady_clock;
        while (auto_run_.load()) {
            int ms = auto_ms_.load();
            if (ms <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
            std::string path;
            {   // re-scan each pass so newly-dropped files join the rotation
                std::lock_guard<std::mutex> lk(mu_);
                scanned_ = false; scan_();
                if (!files_.empty()) { path = files_[auto_idx_ % files_.size()]; ++auto_idx_; }
            }
            if (!path.empty() && emit_file_(path)) {
                std::lock_guard<std::mutex> lk(mu_);
                issued_name_ = fs::path(path).filename().string(); ++issued_count_;
            }
            auto until = clk::now() + std::chrono::milliseconds(ms);   // small chunks: responsive stop / rate change
            while (auto_run_.load() && clk::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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
            .set("auto_ms", auto_ms_.load())
            .set("auto_running", auto_run_.load() && auto_ms_.load() > 0)
            .set("files", files)
            .dump();
    }

    // Small JPEG thumbnail (~140px wide) as a data: URI. Best-effort: empty on fail.
    void make_thumb_(const std::string& path, std::string& out_uri, int& w, int& h) {
        out_uri.clear(); w = h = 0;
        if (!host()) return;
        xi::Image img = decode_file_(path);
        if (img.empty()) return;
        w = img.width; h = img.height;
        const int TW = 140;
        cv::Mat src = xi::as_cv_mat(img);   // view into img (alive for this scope)
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
XI_PLUGIN_PACK_DOOR(LocalImageSource)
