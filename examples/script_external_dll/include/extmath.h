#pragma once
// Tiny external SDK header. Lives under include/ and is found via the project.json
// "include_dirs": ["include"] entry (proves the script-side include hook).
#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllimport) int ext_add(int a, int b);
#ifdef __cplusplus
}
#endif
