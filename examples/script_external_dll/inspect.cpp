//
// inspect.cpp — uses an EXTERNAL DLL (extmath.dll) from the script.
//
//   - <extmath.h> resolves via project.json "include_dirs": ["include"]
//   - extmath.lib is linked via project.json "link_libs": ["extmath.lib"]
//   - extmath.dll is found at runtime because the backend put the project folder
//     on the DLL search path (set_project_dll_search_).
//
#include <xi/xi.hpp>
#include <xi/xi_use.hpp>
#include <extmath.h>

XI_SCRIPT_EXPORT
void xi_inspect_entry(int /*frame*/) {
    const int s = ext_add(2, 3);   // <-- call into the external DLL
    VAR(sum, s);
    VAR(ok, s == 5);
}
