// xi_run_plugin — a headless host mock for a plugin's xi.pack@1 door.
//
// Loads a plugin DLL, stands up the real ImagePool host_api + the host-side
// pack ABI (install_pack_abi — so the plugin's host->get_interface("xi.pack",1)
// resolves exactly as in the backend), creates one instance, optionally applies
// a config (set_def), builds an input pack (named images + scalar metadata),
// drives the plugin's pack door (xi_plugin_get_interface("xi.pack", 1) ->
// xi_pack_proc_v1::process), and dumps the output pack: scalar entries as a
// JSON-ish listing + image entries as PNG files — no VS Code, no WS, no
// backend host required.
//
// THE CUT (ABI v12): the xi::Record data plane (xi_record/xi_record_out/
// xi_plugin_process) is gone. A plugin's sole data plane is the pack door;
// a plugin that doesn't publish it is refused with a clear error.
//
// Usage:
//   xi_run_plugin <plugin.dll> [options]
//     --image <file>            input image entry under key "frame" (repeatable)
//     --image <key>=<file>      input image entry under an explicit key
//     --meta <json>             JSON object; each top-level scalar becomes a
//                               pack entry (int -> i64, real -> f64,
//                               string -> str, bool -> bool). Default "{}".
//     --in-json <json>          alias of --meta (the pre-v12 flag name)
//     --def <json>              set_def config applied before the door runs
//     --out-dir <dir>           where to write output images (default ".")
//     --runs <n>                drive the door n times (stateful plugins; default 1)
//     --dump-def                print the instance's get_def() JSON and exit
//                               (no data plane) — feeds the manifest-param tooling
//
// Images use xInsp2's RGB pool convention: color inputs are loaded with OpenCV
// and BGR->RGB converted (grayscale files stay 1-channel); output image entries
// are RGB->BGR converted before writing. A XI_PACK_NULL door result is a hard
// failure (exit 1); a result pack stamped "$fault" is printed and exits nonzero.

#include <xi/xi_abi.h>
#include <xi/xi_image_pool.hpp>   // ImagePool::make_host_api (the real pool)
#include <xi/xi_pack_abi.hpp>     // install_pack_abi + pack_v1_iface/pack_v4_iface (host pack ABI)
#include <xi/xi_mp.hpp>           // canonical msgpack Writer — the xi/image blob descriptor
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

// Load an image file into the input pack as a self-describing xi/image BLOB
// (spec 30): a grayscale file stays a 1-channel entry (plugins with a "gray"
// contract read it directly); anything else becomes a 3-channel RGB entry (the
// xInsp2 pool convention). The pixels come from a cv::Mat the loader owns, so a
// copy into the pool is inherent — builder_add_blob (the @4 copy convenience)
// synthesizes the head + copies the payload in one call. false on error.
static bool add_image_entry(const xi_pack_v4* fi4, xi_pack_builder b,
                            const std::string& key, const std::string& path,
                            std::string& err) {
    cv::Mat raw = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (raw.empty()) { err = "cannot read image: " + path; return false; }
    cv::Mat out;
    if (raw.channels() == 1)      out = raw.isContinuous() ? raw : raw.clone();
    else if (raw.channels() == 4) cv::cvtColor(raw, out, cv::COLOR_BGRA2RGB);
    else                          cv::cvtColor(raw, out, cv::COLOR_BGR2RGB);
    // {"t":"xi/image","w","h","c","dt"} — the convention descriptor the @1
    // get_image adapter and the SDK image accessors read.
    xi::mp::Writer dw;
    dw.map(5);
    dw.key("t");  dw.str("xi/image");
    dw.key("w");  dw.int_(out.cols);
    dw.key("h");  dw.int_(out.rows);
    dw.key("c");  dw.int_(out.channels());
    dw.key("dt"); dw.str("u8");
    const int64_t payload_len = (int64_t)out.cols * out.rows * out.channels();
    if (!fi4 || fi4->builder_add_blob(b, key.c_str(), dw.bytes().data(),
                                      (int32_t)dw.bytes().size(),
                                      out.data, payload_len) != 1) {
        err = "cannot add image blob (no xi.pack@4 plane?): " + key;
        return false;
    }
    return true;
}

// Save an output image entry (zero-copy view into the result pack) as PNG.
static bool save_image(const xi_pack_image& img, const std::string& path) {
    if (img.width <= 0 || img.height <= 0 || !img.pixels) return false;
    if (img.channels != 1 && img.channels != 3) return false;
    cv::Mat src(img.height, img.width, img.channels == 1 ? CV_8UC1 : CV_8UC3,
                const_cast<void*>(img.pixels));
    cv::Mat bgr;
    if (img.channels == 3) cv::cvtColor(src, bgr, cv::COLOR_RGB2BGR);
    else                   bgr = src;
    return cv::imwrite(path, bgr);
}

// Each --meta top-level scalar becomes a typed pack entry. Nested arrays/
// objects are skipped with a warning (nesting rides msgpack entries, which a
// dev feeds from a real producer, not a CLI flag).
static bool add_meta_entries(const xi_pack_v1* fi, xi_pack_builder b,
                             const std::string& json, std::string& err) {
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) { err = "cannot parse --meta JSON: " + json; return false; }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        err = "--meta must be a JSON object";
        return false;
    }
    yyjson_obj_iter it = yyjson_obj_iter_with(root);
    while (yyjson_val* k = yyjson_obj_iter_next(&it)) {
        yyjson_val* v = yyjson_obj_iter_get_val(k);
        const char* key = yyjson_get_str(k);
        if (yyjson_is_bool(v)) {
            fi->builder_add_bool(b, key, yyjson_get_bool(v) ? 1 : 0);
        } else if (yyjson_is_int(v)) {
            fi->builder_add_i64(b, key, yyjson_get_sint(v));
        } else if (yyjson_is_real(v)) {
            fi->builder_add_f64(b, key, yyjson_get_real(v));
        } else if (yyjson_is_str(v)) {
            fi->builder_add_str(b, key, yyjson_get_str(v),
                                (int32_t)yyjson_get_len(v));
        } else {
            std::fprintf(stderr,
                "warning: --meta key \"%s\" is not a scalar (skipped; nested "
                "data rides msgpack entries, not the CLI)\n", key ? key : "?");
        }
    }
    yyjson_doc_free(doc);
    return true;
}

static void json_escape_into(std::string& out, const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)(unsigned char)c);
                    out += b;
                } else out.push_back(c);
        }
    }
}

static void usage() {
    std::fprintf(stderr,
        "usage: xi_run_plugin <plugin.dll> [--image [key=]file]... "
        "[--meta <json>] [--def <json>] [--out-dir <dir>] [--runs <n>] [--dump-def]\n"
        "\n"
        "Drives the plugin's xi.pack@1 door (ABI v12: the sole data plane).\n"
        "  --image [key=]file  image pack entry (default key \"frame\"; repeatable)\n"
        "  --meta <json>       top-level scalars become i64/f64/str/bool entries\n"
        "                      (--in-json is accepted as an alias)\n"
        "  --def <json>        set_def config applied before the door runs\n"
        "  --out-dir <dir>     where output image entries are written (default .)\n"
        "  --runs <n>          call the door n times (stateful plugins; default 1)\n"
        "  --dump-def          print get_def() JSON and exit (no data plane)\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::string dllpath = argv[1];
    if (dllpath == "--help" || dllpath == "-h") { usage(); return 0; }
    std::vector<std::pair<std::string, std::string>> images;   // (key, file)
    std::string meta = "{}", def, out_dir = ".";
    int runs = 1;
    bool dump_def = false;

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
        } else if (a == "--meta" || a == "--in-json") { meta = need(a.c_str()); }
        else if (a == "--def")       { def     = need("--def"); }
        else if (a == "--out-dir")   { out_dir = need("--out-dir"); }
        else if (a == "--runs")      { runs    = std::atoi(need("--runs").c_str()); if (runs < 1) runs = 1; }
        else if (a == "--dump-def")  { dump_def = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg: %s\n", a.c_str()); usage(); return 2; }
    }

    // The real host surface: pool-backed host_api + the pack ABI published into
    // its get_interface slot ("xi.pack", 1) — exactly what the backend wires.
    static xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_pack_abi();
    const xi_pack_v1* fi  = xi::pack_v1_iface();
    const xi_pack_v4* fi4 = xi::pack_v4_iface();   // spec 30 blob door (image inputs)

#ifdef _WIN32
    HMODULE dll = LoadLibraryA(dllpath.c_str());
    if (!dll) { std::fprintf(stderr, "LoadLibrary failed for %s (err %lu)\n", dllpath.c_str(), GetLastError()); return 2; }
#else
    void* dll = nullptr;   // TODO(linux): dlopen
    std::fprintf(stderr, "xi_run_plugin is Windows-only for now\n"); return 2;
#endif
    // Resolve the plugin's C-ABI exports directly (the canonical symbol names
    // from xi_abi.h). create/destroy are mandatory; the data plane is the
    // xi.pack@1 door behind xi_plugin_get_interface (mandatory unless --dump-def).
    struct Syms {
        xi_plugin_create_fn        create    = nullptr;
        xi_plugin_destroy_fn       destroy   = nullptr;
        xi_plugin_set_def_fn       set_def   = nullptr;
        xi_plugin_get_def_fn       get_def   = nullptr;
        xi_plugin_get_interface_fn get_iface = nullptr;
    } syms;
    syms.create    = (xi_plugin_create_fn)       GetProcAddress(dll, "xi_plugin_create");
    syms.destroy   = (xi_plugin_destroy_fn)      GetProcAddress(dll, "xi_plugin_destroy");
    syms.set_def   = (xi_plugin_set_def_fn)      GetProcAddress(dll, "xi_plugin_set_def");
    syms.get_def   = (xi_plugin_get_def_fn)      GetProcAddress(dll, "xi_plugin_get_def");
    syms.get_iface = (xi_plugin_get_interface_fn)GetProcAddress(dll, "xi_plugin_get_interface");
    if (!syms.create || !syms.destroy) {
        std::fprintf(stderr, "%s: missing required C ABI exports (xi_plugin_create/destroy)\n", dllpath.c_str()); return 2;
    }

    const xi_pack_proc_v1* door = nullptr;
    if (!dump_def) {
        door = syms.get_iface
             ? static_cast<const xi_pack_proc_v1*>(syms.get_iface("xi.pack", 1))
             : nullptr;
        if (!door || !door->process) {
            std::fprintf(stderr,
                "%s: no xi.pack@1 door (xi_plugin_get_interface(\"xi.pack\",1) "
                "unresolved).\nABI v12: the pack door is a plugin's sole data "
                "plane — rebuild the plugin with XI_PLUGIN_PACK_DOOR(YourClass) "
                "after XI_PLUGIN_IMPL.\n", dllpath.c_str());
            return 2;
        }
    }

    void* inst = syms.create(&host, "xi_run_plugin");
    if (!inst) { std::fprintf(stderr, "xi_plugin_create returned null\n"); return 2; }
    if (!def.empty() && syms.set_def) syms.set_def(inst, def.c_str());

    if (dump_def) {
        // Print get_def() JSON and exit — no data plane touched. get_def
        // returns the byte count, or -needed when the buffer is too small.
        std::vector<char> buf(4096);
        int n = syms.get_def ? syms.get_def(inst, buf.data(), (int)buf.size()) : 0;
        if (n < 0) { buf.resize(-n + 1); n = syms.get_def(inst, buf.data(), (int)buf.size()); }
        std::printf("%s\n", (n > 0) ? std::string(buf.data(), n).c_str() : "{}");
        syms.destroy(inst);
#ifdef _WIN32
        FreeLibrary(dll);
#endif
        return 0;
    }

    // ---- build the input pack: image entries + --meta scalars ----------
    xi_pack_builder b = fi->builder_new();
    for (auto& kv : images) {
        std::string err;
        if (!add_image_entry(fi4, b, kv.first, kv.second, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            fi->builder_abandon(b);
            syms.destroy(inst);
            return 2;
        }
    }
    {
        std::string err;
        if (!add_meta_entries(fi, b, meta, err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            fi->builder_abandon(b);
            syms.destroy(inst);
            return 2;
        }
    }
    xi_pack_handle in = fi->builder_seal(b);
    if (in == XI_PACK_NULL) { std::fprintf(stderr, "failed to seal the input pack\n"); syms.destroy(inst); return 2; }

    // ---- drive the door -------------------------------------------------
    int rc = 0;
    xi_pack_handle out = XI_PACK_NULL;
    for (int r = 0; r < runs; ++r) {
        if (out != XI_PACK_NULL) fi->release(out);   // intermediate results
        out = door->process(inst, in);
        if (out == XI_PACK_NULL) {
            std::fprintf(stderr, "pack door returned XI_PACK_NULL on run %d/%d — hard plugin failure\n",
                         r + 1, runs);
            fi->release(in);
            syms.destroy(inst);
#ifdef _WIN32
            FreeLibrary(dll);
#endif
            return 1;
        }
    }

    // ---- dump the result pack: scalars as JSON-ish, images as PNGs ------
    std::string dump = "{";
    std::vector<std::string> saved;
    const int n = fi->count(out);
    bool first = true;
    for (int i = 0; i < n; ++i) {
        int32_t kl = 0;
        const char* kp = fi->key_at(out, i, &kl);
        if (!kp) continue;
        std::string key(kp, (size_t)(kl > 0 ? kl : 0));
        const int tag = fi->tag_at(out, i);
        std::string val;
        switch (tag) {
            case XI_PACK_TAG_I64: {
                int64_t v = 0;
                if (fi->get_i64(out, key.c_str(), &v)) val = std::to_string(v);
                break;
            }
            case XI_PACK_TAG_F64: {
                double v = 0;
                if (fi->get_f64(out, key.c_str(), &v)) {
                    char bufv[64];
                    std::snprintf(bufv, sizeof(bufv), "%.17g", v);
                    val = bufv;
                }
                break;
            }
            case XI_PACK_TAG_BOOL: {
                int32_t v = 0;
                if (fi->get_bool && fi->get_bool(out, key.c_str(), &v)) val = v ? "true" : "false";
                break;
            }
            case XI_PACK_TAG_STR: {
                const char* p = nullptr; int32_t sl = 0;
                if (fi->get_str(out, key.c_str(), &p, &sl)) {
                    val = "\"";
                    json_escape_into(val, p, (size_t)(sl > 0 ? sl : 0));
                    val += "\"";
                }
                break;
            }
            case XI_PACK_TAG_BIN: {
                const void* p = nullptr; int32_t bl = 0;
                if (fi->get_bin(out, key.c_str(), &p, &bl))
                    val = "{\"$bin_bytes\":" + std::to_string(bl) + "}";
                break;
            }
            case XI_PACK_TAG_MP: {
                const void* p = nullptr; int32_t ml = 0;
                if (fi->get_mp(out, key.c_str(), &p, &ml))
                    val = "{\"$msgpack_bytes\":" + std::to_string(ml) + "}";
                break;
            }
            case XI_PACK_TAG_BLOB: {
                // spec 30: a non-scalar entry is a self-describing blob. An
                // "xi/image" blob parses through the @1 get_image adapter into
                // an xi_pack_image we can save as PNG; any other blob type is
                // reported by its payload size (get_blob), not written out.
                xi_pack_image img{};
                if (fi->get_image(out, key.c_str(), &img)) {
                    std::string path = out_dir + "/" + key + ".png";
                    if (save_image(img, path)) saved.push_back(path);
                    val = "{\"$image\":[" + std::to_string(img.width) + "," +
                          std::to_string(img.height) + "," +
                          std::to_string(img.channels) + "]}";
                } else if (fi4) {
                    const void* dp = nullptr; int32_t dl = 0;
                    const void* pp = nullptr; int64_t pl = 0;
                    if (fi4->get_blob(out, key.c_str(), &dp, &dl, &pp, &pl))
                        val = "{\"$blob_bytes\":" + std::to_string(pl) + "}";
                }
                break;
            }
            default: break;
        }
        if (val.empty()) continue;
        if (!first) dump += ",";
        dump += "\n  \"";
        json_escape_into(dump, key.data(), key.size());
        dump += "\": " + val;
        first = false;
    }
    dump += first ? "}" : "\n}";

    std::printf("=== output pack (%d entr%s) ===\n%s\n", n, n == 1 ? "y" : "ies", dump.c_str());
    if (!saved.empty()) {
        std::printf("=== output images (%zu) ===\n", saved.size());
        for (auto& p : saved) std::printf("  %s\n", p.c_str());
    }

    // Fail-loud contract: a "$fault"-stamped result is a routed failure.
    {
        const char* fc = nullptr; int32_t fl = 0;
        if (fi->get_str(out, "$fault", &fc, &fl)) {
            std::string reason(fc, (size_t)(fl > 0 ? fl : 0));
            std::string detail;
            const char* dp = nullptr; int32_t dl = 0;
            if (fi->get_str(out, "$fault_detail", &dp, &dl))
                detail = " — " + std::string(dp, (size_t)(dl > 0 ? dl : 0));
            std::fprintf(stderr, "plugin faulted: $fault=%s%s\n", reason.c_str(), detail.c_str());
            rc = 1;
        }
    }

    fi->release(out);
    fi->release(in);
    syms.destroy(inst);
#ifdef _WIN32
    FreeLibrary(dll);
#endif
    return rc;
}
