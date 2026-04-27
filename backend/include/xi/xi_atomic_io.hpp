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
#ifdef _WIN32
    // Use Win32 directly so we can FlushFileBuffers before rename.
    // std::ofstream::flush() only flushes user-space buffers — it
    // does NOT push them to disk. MOVEFILE_WRITE_THROUGH on the
    // rename flushes the *directory entry change*, not the file
    // contents. Without an explicit FlushFileBuffers(handle) here,
    // a power loss between the close and the rename can leave the
    // .tmp file with metadata committed but contents zero / torn,
    // and the rename then makes a corrupt file the canonical one.
    HANDLE h = CreateFileW(
        tmp.wstring().c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    if (!WriteFile(h, content.data(),
                   (DWORD)content.size(), &wrote, nullptr)
        || wrote != (DWORD)content.size()) {
        CloseHandle(h);
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    if (!FlushFileBuffers(h)) {
        // Best-effort — if flush fails (e.g., disk full), bail and
        // don't rename the bad data over the good.
        CloseHandle(h);
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    CloseHandle(h);
    if (!MoveFileExW(tmp.wstring().c_str(), path.wstring().c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
#else
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(content.data(), (std::streamsize)content.size());
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
        f.flush();
    }
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
