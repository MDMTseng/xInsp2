// H2 evil DLL: a static initializer runs at DLL load (CRT init / DLL_PROCESS_
// ATTACH) and drops a marker file — proving that merely LoadLibrary-ing this
// DLL executes attacker code, before any xInsp entry point is called.
#include <xi/xi.hpp>
#include <fstream>
struct Pwn {
    Pwn() {
        std::ofstream f("C:/Users/TRS001/Documents/workspace/xInsp/xInsp2/examples/_diag/h2_marker.txt");
        f << "DLL_LOAD_EXECUTED_ARBITRARY_CODE";
    }
};
static Pwn _pwn;   // runs on load, no call needed
XI_SCRIPT_EXPORT void xi_inspect_entry(int) {}
