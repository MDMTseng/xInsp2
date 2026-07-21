//
// crash_breakpad.cpp — Breakpad-backed minidump writer.
//
// Compiled ONLY when the build sets -DXINSP2_HAS_BREAKPAD (see backend/CMakeLists
// + docs/roadmap/linux-port.md), and linked ONLY into xinsp-backend + xinsp-runner.
// It is the single translation unit that depends on Google Breakpad, so the rest
// of the tree (and every test that includes xi_crash_dump.hpp) never links it.
//
// It registers a MinidumpWriterFn into xi::crash's writer hook; the portable
// crash handler (xi_crash_dump.hpp, POSIX path) calls it from the terminate /
// SIGABRT path to produce a REAL minidump (all threads + memory + registers,
// analyzable with minidump_stackwalk) instead of the text-backtrace fallback.
//
// The minidump is written ON DEMAND from the terminate handler (not from the
// fatal-signal handler): the xi_seh.hpp translator owns SIGSEGV/FPE/BUS/ILL so a
// plugin fault becomes a catchable exception (the crash-isolation model). Only an
// UNCAUGHT fault reaches std::terminate here — so the faulting thread's stack is
// unwound to the terminate frame; precise fault-site module blame is preserved
// separately via xi::last_fault_addr() in the .json sidecar.
//
#include <xi/xi_crash_dump.hpp>

#include "client/linux/handler/exception_handler.h"
#include "client/linux/handler/minidump_descriptor.h"

#include <cstring>

namespace {

struct DumpResult { char* out; int cap; bool ok; };

// Breakpad calls this once the minidump is written; capture the .dmp path.
bool on_minidump(const google_breakpad::MinidumpDescriptor& desc,
                 void* context, bool succeeded) {
    auto* r = static_cast<DumpResult*>(context);
    if (succeeded && desc.path() && r->cap > 0) {
        std::strncpy(r->out, desc.path(), (size_t)r->cap - 1);
        r->out[r->cap - 1] = '\0';
        r->ok = true;
    }
    return succeeded;
}

// Matches xi::crash::MinidumpWriterFn: write a minidump into `dir`, fill `out`
// with its path, return success. WriteMinidump self-clones + ptraces to capture
// every thread's live state — safe to call from the terminate handler.
bool write_minidump_breakpad(const char* dir, char* out, int cap) {
    if (!out || cap <= 0) return false;
    out[0] = '\0';
    DumpResult r{ out, cap, false };
    google_breakpad::ExceptionHandler::WriteMinidump(std::string(dir), on_minidump, &r);
    return r.ok;
}

} // namespace

namespace xi {
namespace crash {

// Declared in xi_crash_dump.hpp (guarded by XINSP2_HAS_BREAKPAD). Call once at
// boot before install(); wires the Breakpad writer into the crash handler.
void register_breakpad_writer() {
    set_minidump_writer(&write_minidump_breakpad);
}

} // namespace crash
} // namespace xi
