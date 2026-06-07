// C2 PoC slow script: process() sleeps for several seconds so a concurrent
// compile_and_load can FreeLibrary this DLL while xi_inspect_entry is still
// executing inside it. When the sleep returns, the function epilogue (and the
// VAR below) run from a module that may already be unmapped -> UAF.
#include <xi/xi.hpp>
#include <thread>
#include <chrono>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    VAR(phase, std::string("enter-slow-inspect"));
    // long enough for a reload's cl.exe compile + FreeLibrary to land mid-run
    std::this_thread::sleep_for(std::chrono::milliseconds(6000));
    // executed from THIS dll's code after the sleep — UAF if unloaded
    VAR(phase2, std::string("survived-sleep"));
}
