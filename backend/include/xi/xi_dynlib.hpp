#pragma once
//
// xi_dynlib.hpp — cross-platform dynamic-module loader shim.
//
// The backend loads compiled scripts + plugins as shared modules and resolves
// their C-ABI entry points by name. On Windows that is LoadLibrary/GetProcAddress/
// FreeLibrary over an HMODULE; on POSIX it is dlopen/dlsym/dlclose over a void*.
//
// Rather than sprinkle #ifdef _WIN32 across every loader call site (the loader
// family — xi_script_loader, xi_cabi_adapter, xi_pm_*), this header is a DROP-IN
// replacement for the `#include <windows.h>` those files used to do:
//
//   * On Windows it just pulls in <windows.h> (LEAN + NOMINMAX), so every native
//     LoadLibraryExA / GetProcAddress / FreeLibrary / HMODULE keeps its real type.
//   * On POSIX it defines the SAME NAMES (HMODULE, LoadLibraryExA, LoadLibraryA,
//     GetProcAddress, FreeLibrary, GetModuleFileNameA, DWORD, MAX_PATH, the
//     LOAD_LIBRARY_SEARCH_* flags) as thin inline wrappers over <dlfcn.h>, so the
//     existing call sites compile and run UNCHANGED.
//
// This is a deliberate compatibility shim (keeps the Windows code path pristine
// and the diff small), not a new abstraction the rest of the code is expected to
// adopt. New portable code can just call these names; they mean the same thing on
// both platforms. Semantics that DON'T map are documented at each wrapper.
//

#if defined(_WIN32)

#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>

#else // ---- POSIX (Linux / macOS) -------------------------------------------

#  include <dlfcn.h>
#  include <climits>
#  include <cstdint>
#  include <cstring>
#  include <string>
#  include <unistd.h>     // getpid; readlink for /proc/self/exe
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>  // _NSGetExecutablePath
#  endif

// Win32 spellings the loader call sites use verbatim.
using HMODULE = void*;
using DWORD   = std::uint32_t;

#  ifndef MAX_PATH
#    define MAX_PATH 4096
#  endif

// LoadLibraryEx search-path flags have no POSIX analogue (RPATH/$ORIGIN and the
// ld.so search order do that job). Defined as 0 so the flag arithmetic at the
// call sites is inert.
#  ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#    define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0
#  endif
#  ifndef LOAD_LIBRARY_SEARCH_USER_DIRS
#    define LOAD_LIBRARY_SEARCH_USER_DIRS 0
#  endif
#  ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#    define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0
#  endif
#  ifndef GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT
#    define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 0
#  endif

// AddDllDirectory returns an opaque cookie the caller later feeds to
// RemoveDllDirectory. No ld.so analogue — the DLL-search-dir mechanism is inert
// on POSIX — so the cookie is just a nullable void*.
using DLL_DIRECTORY_COOKIE = void*;

namespace xi::dynlib_detail {

// Last dlerror() text, captured per-thread on the failing call. The Win32 loader
// call sites build their diagnostic from GetLastError()'s numeric code (which has
// no dlerror equivalent); this preserves the richer text for anyone who wants it
// via xi::dynlib_detail::last_error().
inline std::string& tls_error() {
    thread_local std::string e;
    return e;
}

} // namespace xi::dynlib_detail

namespace xi { inline const std::string& dynlib_last_error() { return dynlib_detail::tls_error(); } }

// dlopen with immediate binding + local scope: matches Windows load-time symbol
// resolution and keeps a plugin's symbols out of the global namespace (the host
// hands plugins its API via the xi_host_api pointer, never via dlsym), so two
// plugins that happen to share a symbol name can't collide.
inline HMODULE LoadLibraryExA(const char* path, void* /*reserved*/, DWORD /*flags*/) {
    ::dlerror(); // clear
    HMODULE h = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = ::dlerror();
        xi::dynlib_detail::tls_error() = e ? e : "dlopen failed";
    }
    return h;
}

inline HMODULE LoadLibraryA(const char* path) {
    return LoadLibraryExA(path, nullptr, 0);
}

inline void* GetProcAddress(HMODULE h, const char* name) {
    return h ? ::dlsym(h, name) : nullptr;
}

// Win32 FreeLibrary returns nonzero on success; dlclose returns 0 on success —
// invert so the call sites' truthiness (and the rare `if (!FreeLibrary(...))`)
// keeps the Windows meaning.
inline int FreeLibrary(HMODULE h) {
    return (h && ::dlclose(h) == 0) ? 1 : 0;
}

// Refcount-neutral "is a module of this name currently mapped?" query. Win32 uses
// GetModuleHandleExA(UNCHANGED_REFCOUNT); dlopen(RTLD_NOLOAD) returns the handle
// iff already loaded — but still bumps the refcount, so we drop it immediately.
// Callers only null-check *out, never dereference it, so handing back the
// (now-balanced) handle is safe.
inline int GetModuleHandleExA(DWORD /*flags*/, const char* name, HMODULE* out) {
    void* h = ::dlopen(name, RTLD_NOLOAD | RTLD_NOW);
    if (out) *out = h;
    if (h) ::dlclose(h); // undo the RTLD_NOLOAD refcount bump
    return h ? 1 : 0;
}

// The DLL-search-dir mechanism has no ld.so analogue; these are inert no-ops so
// the open_project path compiles and runs (dependency resolution goes through
// RPATH / LD_LIBRARY_PATH instead).
inline DLL_DIRECTORY_COOKIE AddDllDirectory(const wchar_t* /*dir*/) { return nullptr; }
inline int RemoveDllDirectory(DLL_DIRECTORY_COOKIE /*cookie*/) { return 1; }

inline DWORD GetCurrentProcessId() { return (DWORD)::getpid(); }

inline DWORD GetLastError() {
    // No numeric errno analogue for the dl* family; the useful detail is the
    // string, exposed via xi::dynlib_last_error(). Callers only stringify this.
    return 0;
}

// GetModuleFileNameA(nullptr, buf, size): the path of the running executable.
// (The module-handle form used only by crash forensics — dladdr — is not needed
// here; crash_dump keeps its own #ifdef.) Returns the length written, 0 on error,
// matching the Win32 contract the call sites check.
inline DWORD GetModuleFileNameA(HMODULE mod, char* buf, DWORD size) {
    if (mod == nullptr) {
#  if defined(__linux__)
        ssize_t n = ::readlink("/proc/self/exe", buf, (size_t)size - 1);
        if (n <= 0) return 0;
        buf[n] = '\0';
        return (DWORD)n;
#  elif defined(__APPLE__)
        uint32_t bufsize = size;
        if (_NSGetExecutablePath(buf, &bufsize) != 0) return 0;
        return (DWORD)std::strlen(buf);
#  else
        return 0;
#  endif
    }
    // Module-handle form: resolve via dladdr when a symbol is knowable. Not used
    // by the portable call sites; return 0 (caller treats as failure).
    (void)mod; (void)buf; (void)size;
    return 0;
}

#endif // _WIN32

#include <string>

// Map a plugin manifest's Windows-convention module name ("xi-<name>.dll") to the
// platform's actual shared-object file. Plugin manifests (plugin.json "dll") and
// the plugins/ CMake share the SAME "xi-" prefix on every platform
// (CMAKE_SHARED_LIBRARY_PREFIX="xi-"), so only the SUFFIX differs: .dll on
// Windows, .so on Linux, .dylib on macOS. A path that already carries the native
// suffix (e.g. a JIT-compiled script/plugin .so) is returned unchanged, so this
// is safe to apply at every module-load site. Use it wherever a manifest-derived
// module path is handed to the loader (load_plugin_dll_, the certify child).
inline std::string platform_module_path(const std::string& path) {
#if defined(_WIN32)
    return path;   // manifests already name the .dll
#else
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".dll") == 0)
#  if defined(__APPLE__)
        return path.substr(0, path.size() - 4) + ".dylib";
#  else
        return path.substr(0, path.size() - 4) + ".so";
#  endif
    return path;
#endif
}
