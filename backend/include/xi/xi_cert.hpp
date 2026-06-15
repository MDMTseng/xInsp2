#pragma once
//
// xi_cert.hpp — read/write/validate plugin certification files.
//
// A cert proves a specific DLL passed baseline tests at baseline version N.
// The host writes cert.json next to plugin.json on successful certification
// and reads it on load to decide whether to skip re-testing.
//
// Cert is invalidated if ANY of:
//   - DLL SHA-256 changed (anything in the file is different)
//   - baseline_version in cert < current BASELINE_VERSION
//
// Tamper detection used to be (file_size, mtime) — `touch` plus a same-size
// rebuild bypassed it (deep architecture review 2026-04-28). SHA-256
// of the DLL bytes is now the canonical fingerprint; size/mtime are
// retained as informational fields only.
//
// cert_format_version distinguishes the new schema from the legacy one
// so older certs (no `dll_sha256` field) are forced through re-cert
// rather than silently treated as valid.
//

#include "xi_atomic_io.hpp"
#include "xi_baseline.hpp"
#include "xi_sha256.hpp"
#include "yyjson.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace xi::cert {

// Bumped when the cert.json schema changes incompatibly. Old certs
// missing required new fields are treated as invalid (forces re-cert).
constexpr int CERT_FORMAT_VERSION = 2;   // v1 = (size, mtime); v2 = SHA-256

struct Cert {
    std::string              plugin_name;
    int                      cert_format_version = 0;
    std::string              dll_sha256;       // 64-hex; canonical fingerprint
    int64_t                  dll_size         = 0;   // informational
    int64_t                  dll_mtime        = 0;   // informational
    int                      baseline_version = 0;
    std::string              certified_at;     // ISO 8601
    double                   duration_ms      = 0;
    std::vector<std::string> tests_passed;
};

inline int64_t dll_mtime_of(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    auto dur = t.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(dur).count();
}

inline int64_t dll_size_of(const std::filesystem::path& p) {
    std::error_code ec;
    auto s = std::filesystem::file_size(p, ec);
    return ec ? 0 : (int64_t)s;
}

inline std::string iso8601_now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    // D-P1-3: std::gmtime returns a pointer to a static-storage tm
    // shared across all callers — racy when two threads cert plugins
    // concurrently. The thread-safe variant differs by platform.
#ifdef _WIN32
    struct tm tm_buf;
    if (gmtime_s(&tm_buf, &t) != 0) return "1970-01-01T00:00:00Z";
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
#else
    struct tm tm_buf;
    if (!gmtime_r(&t, &tm_buf)) return "1970-01-01T00:00:00Z";
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
#endif
    return buf;
}

inline std::filesystem::path cert_path(const std::filesystem::path& plugin_folder) {
    return plugin_folder / "cert.json";
}

inline bool write(const std::filesystem::path& plugin_folder, const Cert& c) {
    yyjson_mut_doc* d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val* root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "plugin_name",         c.plugin_name.c_str());
    yyjson_mut_obj_add_real(d, root, "cert_format_version", (double)c.cert_format_version);
    yyjson_mut_obj_add_strcpy(d, root, "dll_sha256",          c.dll_sha256.c_str());
    yyjson_mut_obj_add_real(d, root, "dll_size",            (double)c.dll_size);
    yyjson_mut_obj_add_real(d, root, "dll_mtime",           (double)c.dll_mtime);
    yyjson_mut_obj_add_real(d, root, "baseline_version",    (double)c.baseline_version);
    yyjson_mut_obj_add_strcpy(d, root, "certified_at",        c.certified_at.c_str());
    yyjson_mut_obj_add_real(d, root, "duration_ms",         c.duration_ms);
    yyjson_mut_val* arr = yyjson_mut_arr(d);
    for (auto& t : c.tests_passed) yyjson_mut_arr_add_val(arr, yyjson_mut_strcpy(d, t.c_str()));
    yyjson_mut_obj_add_val(d, root, "tests_passed", arr);
    yyjson_mut_doc_set_root(d, root);
    char* s = yyjson_mut_write(d, YYJSON_WRITE_PRETTY, NULL);
    bool ok = xi::atomic_write(cert_path(plugin_folder), std::string(s ? s : ""));
    std::free(s);
    yyjson_mut_doc_free(d);
    return ok;
}

inline bool read(const std::filesystem::path& plugin_folder, Cert& out) {
    std::ifstream f(cert_path(plugin_folder).string());
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    yyjson_doc* doc = yyjson_read(content.c_str(), content.size(), 0);
    yyjson_val* root = doc ? yyjson_doc_get_root(doc) : nullptr;
    if (!root) { if (doc) yyjson_doc_free(doc); return false; }
    auto str = [&](const char* k, std::string& dst) {
        yyjson_val* j = yyjson_obj_get(root, k);
        if (j && yyjson_is_str(j)) dst = yyjson_get_str(j);
    };
    auto num = [&](const char* k, auto& dst) {
        yyjson_val* j = yyjson_obj_get(root, k);
        if (j && yyjson_is_num(j)) dst = (std::decay_t<decltype(dst)>)yyjson_get_num(j);
    };
    str("plugin_name",         out.plugin_name);
    num("cert_format_version", out.cert_format_version);
    str("dll_sha256",          out.dll_sha256);
    num("dll_size",            out.dll_size);
    num("dll_mtime",           out.dll_mtime);
    num("baseline_version",    out.baseline_version);
    str("certified_at",        out.certified_at);
    num("duration_ms",         out.duration_ms);
    yyjson_doc_free(doc);
    return true;
}

// Is the existing cert (if any) still valid for the given DLL at the
// current baseline version? Validity is keyed off the SHA-256 of the
// DLL bytes, not (size, mtime) — `touch` plus a same-size rebuild
// would otherwise pass the old check.
//
// Legacy v1 certs (no `dll_sha256` / `cert_format_version` field)
// are treated as invalid → caller re-runs the baseline. Cheap one-
// time cost; no false positives from a stale fingerprint.
inline bool is_valid(const std::filesystem::path& plugin_folder,
                     const std::filesystem::path& dll_path)
{
    Cert c;
    if (!read(plugin_folder, c)) return false;
    if (c.cert_format_version != CERT_FORMAT_VERSION) return false;
    if (c.baseline_version    != xi::baseline::BASELINE_VERSION) return false;
    if (c.dll_sha256.empty()) return false;
    auto fresh = xi::sha256::sha256_file(dll_path.string());
    if (fresh.empty()) return false;
    if (fresh != c.dll_sha256) return false;
    return true;
}

// Run baseline tests on a loaded DLL; on success, write cert.json.
// Returns the full summary either way so the caller can log failures.
inline baseline::Summary certify(const std::filesystem::path& plugin_folder,
                                 const std::filesystem::path& dll_path,
                                 const std::string&           plugin_name,
                                 const baseline::PluginSymbols& syms,
                                 const xi_host_api*            host)
{
    auto summary = baseline::run_all(syms, host);
    if (summary.all_passed) {
        Cert c;
        c.plugin_name         = plugin_name;
        c.cert_format_version = CERT_FORMAT_VERSION;
        c.dll_sha256          = xi::sha256::sha256_file(dll_path.string());
        c.dll_size            = dll_size_of(dll_path);
        c.dll_mtime           = dll_mtime_of(dll_path);
        c.baseline_version    = baseline::BASELINE_VERSION;
        c.certified_at        = iso8601_now();
        c.duration_ms         = summary.total_ms;
        for (auto& r : summary.results) c.tests_passed.push_back(r.name);
        write(plugin_folder, c);
    }
    return summary;
}

} // namespace xi::cert
