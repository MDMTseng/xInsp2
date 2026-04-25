#pragma once
//
// xi_seh.hpp — convert Windows SEH (access violation, divide-by-zero, etc.)
// into C++ exceptions catchable by ordinary try/catch.
//
// Install once per worker thread (the translator is per-thread on MSVC):
//
//     _set_se_translator(xi::seh_translator);
//
// Then:
//
//     try { plugin_call(); }
//     catch (const xi::seh_exception& e) {
//         // e.code is the Win32 SEH code, e.what() is a short name
//     }
//
// Helpers `xi::spawn_worker` (xi_thread.hpp) and `xi::async` (xi_async.hpp)
// install this translator automatically at thread entry.
//
// MSVC-only — no-op everywhere else.
//

#include <exception>

#ifdef _MSC_VER
#include <eh.h>
#endif

namespace xi {

class seh_exception : public std::exception {
public:
    unsigned int code;
    explicit seh_exception(unsigned int c) : code(c) {}
    const char* what() const noexcept override {
        switch (code) {
            case 0xC0000005: return "ACCESS_VIOLATION";
            case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
            case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
            case 0xC00000FD: return "STACK_OVERFLOW";
            case 0xC000001D: return "ILLEGAL_INSTRUCTION";
            case 0xC0000090: return "FLOAT_INVALID_OPERATION";
            case 0xC0000091: return "FLOAT_DIVIDE_BY_ZERO";
            case 0xC0000096: return "PRIVILEGED_INSTRUCTION";
            case 0xC00000FE: return "INVALID_DISPOSITION";
            default:         return "UNKNOWN_SEH_EXCEPTION";
        }
    }
};

#ifdef _MSC_VER
inline void seh_translator(unsigned int code, struct _EXCEPTION_POINTERS*) {
    throw seh_exception(code);
}

inline void install_seh_translator() {
    _set_se_translator(seh_translator);
}
#else
inline void install_seh_translator() {}
#endif

} // namespace xi
