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
#include "cJSON.h"

#include <cstdint>
#include <cstdio>
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
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

inline std::filesystem::path cert_path(const std::filesystem::path& plugin_folder) {
    return plugin_folder / "cert.json";
}

inline bool write(const std::filesystem::path& plugin_folder, const Cert& c) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "plugin_name",         c.plugin_name.c_str());
    cJSON_AddNumberToObject(root, "cert_format_version", (double)c.cert_format_version);
    cJSON_AddStringToObject(root, "dll_sha256",          c.dll_sha256.c_str());
    cJSON_AddNumberToObject(root, "dll_size",            (double)c.dll_size);
    cJSON_AddNumberToObject(root, "dll_mtime",           (double)c.dll_mtime);
    cJSON_AddNumberToObject(root, "baseline_version",    (double)c.baseline_version);
    cJSON_AddStringToObject(root, "certified_at",        c.certified_at.c_str());
    cJSON_AddNumberToObject(root, "duration_ms",         c.duration_ms);
    cJSON* arr = cJSON_CreateArray();
    for (auto& t : c.tests_passed) cJSON_AddItemToArray(arr, cJSON_CreateString(t.c_str()));
    cJSON_AddItemToObject(root, "tests_passed", arr);
    char* s = cJSON_Print(root);
    bool ok = xi::atomic_write(cert_path(plugin_folder), std::string(s ? s : ""));
    std::free(s);
    cJSON_Delete(root);
    return ok;
}

inline bool read(const std::filesystem::path& plugin_folder, Cert& out) {
    std::ifstream f(cert_path(plugin_folder).string());
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;
    auto str = [&](const char* k, std::string& dst) {
        cJSON* j = cJSON_GetObjectItem(root, k);
        if (j && cJSON_IsString(j)) dst = j->valuestring;
    };
    auto num = [&](const char* k, auto& dst) {
        cJSON* j = cJSON_GetObjectItem(root, k);
        if (j && cJSON_IsNumber(j)) dst = (decltype(dst))j->valuedouble;
    };
    str("plugin_name",         out.plugin_name);
    num("cert_format_version", out.cert_format_version);
    str("dll_sha256",          out.dll_sha256);
    num("dll_size",            out.dll_size);
    num("dll_mtime",           out.dll_mtime);
    num("baseline_version",    out.baseline_version);
    str("certified_at",        out.certified_at);
    num("duration_ms",         out.duration_ms);
    cJSON_Delete(root);
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
