// xi_run_plugin — a headless host mock for a plugin's process().
//
// Loads a plugin DLL, stands up the real ImagePool host_api (so pool_image and
// the image getters work exactly as in the backend), creates one instance,
// optionally applies a config (set_def), feeds it an input Record (JSON + named
// images), calls process(), and dumps the output Record JSON + images — no VS
// Code, no WS, no backend host required.
//
// Usage:
//   xi_run_plugin <plugin.dll> [options]
//     --image <file>            input image under key "frame" (repeatable)
//     --image <key>=<file>      input image under an explicit key
//     --in-json <json>          input Record JSON (default "{}")
//     --def <json>              set_def config applied before process()
//     --out-dir <dir>           where to write output images (default ".")
//     --runs <n>                call process() n times (stateful plugins; default 1)
//
// Images use xInsp2's RGB pool convention: inputs are loaded with OpenCV and
// BGR->RGB converted; output images are RGB->BGR converted before writing.
// Output Record JSON is read from out_doc (in-process doc path) or data/len.

#include <xi/xi_abi.hpp>
#include <xi/xi_baseline.hpp>     // load_symbols
#include <xi/xi_image_pool.hpp>   // ImagePool::make_host_api
#include "yyjson.h"

#include <opencv2/opencv.hpp>

#ifdef _WIN32
  #include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

static std::string read_out_json(const xi_record_out& out) {
    if (out.out_doc) {
        size_t n = 0;
        char* s = yyjson_mut_write(reinterpret_cast<yyjson_mut_doc*>(out.out_doc),
                                   YYJSON_WRITE_PRETTY, &n);
        std::string r = s ? std::string(s, n) : "";
        if (s) free(s);
        return r;
    }
    return out.data ? std::string(reinterpret_cast<const char*>(out.data),
                                  static_cast<size_t>(out.len)) : "";
}

// Load an image file into the pool as RGB (the xInsp2 convention). Returns 0 on error.
static xi_image_handle load_image(const xi_host_api& host, const std::string& path, std::string& err) {
    cv::Mat bgr = cv::imread(path, cv::IMREAD_COLOR);   // 3-channel BGR
    if (bgr.empty()) { err = "cannot read image: " + path; return 0; }
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    xi_image_handle h = host.image_create(rgb.cols, rgb.rows, 3);
    if (!h) { err = "image_create failed"; return 0; }
    cv::Mat dst(rgb.rows, rgb.cols, CV_8UC3, host.image_data(h), host.image_stride(h));
    rgb.copyTo(dst);   // copyTo honours the pool stride
    return h;
}

static void save_image(const xi_host_api& host, xi_image_handle h, const std::string& path) {
    int w = host.image_width(h), ht = host.image_height(h);
    int ch = host.image_channels(h), st = host.image_stride(h);
    if (w <= 0 || ht <= 0 || !host.image_data(h)) return;
    cv::Mat src(ht, w, ch == 1 ? CV_8UC1 : CV_8UC3, host.image_data(h), st);
    cv::Mat bgr;
    if (ch == 3) cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
    else         bgr = src;
    cv::imwrite(path, bgr);
}

static void usage() {
    std::fprintf(stderr,
        "usage: xi_run_plugin <plugin.dll> [--image [key=]file]... "
        "[--in-json <json>] [--def <json>] [--out-dir <dir>] [--runs <n>]\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string dllpath = argv[1];
    std::vector<std::pair<std::string, std::string>> images;   // (key, file)
    std::string in_json = "{}", def, out_dir = ".";
    int runs = 1;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--image") {
            std::string v = need("--image");
            auto eq = v.find('=');
            if (eq != std::string::npos) images.emplace_back(v.substr(0, eq), v.substr(eq + 1));
            else                         images.emplace_back("frame", v);
        } else if (a == "--in-json") { in_json = need("--in-json"); }
        else if (a == "--def")       { def     = need("--def"); }
        else if (a == "--out-dir")   { out_dir = need("--out-dir"); }
        else if (a == "--runs")      { runs    = std::atoi(need("--runs").c_str()); if (runs < 1) runs = 1; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); return 2; }
    }

    xi_host_api host = xi::ImagePool::make_host_api();

#ifdef _WIN32
    HMODULE dll = LoadLibraryA(dllpath.c_str());
    if (!dll) { std::fprintf(stderr, "LoadLibrary failed for %s (err %lu)\n", dllpath.c_str(), GetLastError()); return 2; }
#else
    void* dll = nullptr;   // TODO(linux): dlopen
    std::fprintf(stderr, "xi_run_plugin is Windows-only for now\n"); return 2;
#endif
    auto syms = xi::baseline::load_symbols(dll);
    if (!syms.ok() || !syms.process) { std::fprintf(stderr, "%s: missing required C ABI exports\n", dllpath.c_str()); return 2; }

    void* inst = syms.create(&host, "xi_run_plugin");
    if (!inst) { std::fprintf(stderr, "xi_plugin_create returned null\n"); return 2; }
    if (!def.empty() && syms.set_def) syms.set_def(inst, def.c_str());

    std::vector<xi_record_image> recimgs;
    recimgs.reserve(images.size());
    for (auto& kv : images) {
        std::string err;
        xi_image_handle h = load_image(host, kv.second, err);
        if (!h) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        recimgs.push_back({ kv.first.c_str(), h });   // key string stays alive in `images`
    }

    xi_record in{};
    in.images      = recimgs.empty() ? nullptr : recimgs.data();
    in.image_count = static_cast<int32_t>(recimgs.size());
    in.data        = reinterpret_cast<const uint8_t*>(in_json.c_str());
    in.len         = static_cast<int32_t>(in_json.size());

    std::string final_json;
    std::vector<std::string> saved;
    for (int r = 0; r < runs; ++r) {
        xi_record_out out;
        xi_record_out_init(&out);
        syms.process(inst, &in, &out);
        if (r == runs - 1) {
            final_json = read_out_json(out);
            for (int i = 0; i < out.image_count; ++i) {
                std::string key = out.images[i].key ? out.images[i].key : ("out" + std::to_string(i));
                std::string path = out_dir + "/" + key + ".png";
                save_image(host, out.images[i].handle, path);   // BEFORE free
                saved.push_back(path);
            }
        }
        xi_record_out_free(&out);
    }

    std::printf("=== output record JSON ===\n%s\n", final_json.c_str());
    if (!saved.empty()) {
        std::printf("=== output images (%zu) ===\n", saved.size());
        for (auto& p : saved) std::printf("  %s\n", p.c_str());
    }

    syms.destroy(inst);
#ifdef _WIN32
    FreeLibrary(dll);
#endif
    return 0;
}
