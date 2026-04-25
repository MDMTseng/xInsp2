#pragma once
//
// xi_atomic_io.hpp — crash-safe file writes.
//
// `atomic_write(path, content)` writes to `<path>.tmp` first, then
// atomically renames over the destination. If the process dies between
// the open() and the rename(), the original file is unchanged and a
// `.tmp` orphan is left behind (cosmetic — startup validation can sweep).
//
// On Windows, `MoveFileExW` with `MOVEFILE_REPLACE_EXISTING` is the
// closest available primitive — atomic for the metadata flip even if
// not strictly POSIX rename(2).
//
// Use this for every JSON / config / manifest write that other code
// reads back later. Don't use it for streaming output (e.g., raw
// pixel files in recording sessions); those write big buffers
// directly and tolerate truncation.
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace xi {

inline bool atomic_write(const std::filesystem::path& path,
                         std::string_view content) {
    namespace fs = std::filesystem;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), (std::streamsize)content.size());
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
        // Force flush so a process crash AFTER close() but before
        // rename() doesn't leave a half-written tmp masquerading as
        // complete.
        f.flush();
    }
#ifdef _WIN32
    // MoveFileExW with REPLACE_EXISTING: atomic on the same volume,
    // overwrites destination. Convert paths through native string.
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
#else
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
#endif
}

inline bool atomic_write(const std::filesystem::path& path,
                         const std::string& content) {
    return atomic_write(path, std::string_view(content));
}

} // namespace xi
