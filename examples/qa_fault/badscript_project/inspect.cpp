//
// inspect.cpp — qa_fault_badscript (AS-I6 fixture): DELIBERATELY WON'T COMPILE.
//
// The autostart's compile_and_load must fail with a compile error and the
// backend must STAY UP (a bad project on a line degrades safely; it does not
// take the inspector process down). The errors below are intentional:
//   - unknown type `frmae` (typo)
//   - call to an undeclared function
//   - missing semicolon / unbalanced braces
// Do NOT "fix" this file — it is a negative fixture. See examples/qa_fault/PLAN.md.
//
#include <xi/xi.hpp>

XI_SCRIPT_EXPORT
void xi_inspect_entry(frmae) {          // 'frmae' is not a type
    this_function_does_not_exist(frame) // undeclared + missing ';'
    VAR(broken, 1)
    // intentionally unbalanced — missing closing brace below
