//
// test_hang_plugin.cpp — minimal plugin that intentionally hangs in
// process(). Used by test_worker_timeout to verify the watchdog
// CancelIoEx path actually unblocks a stuck call.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <xi/xi_abi.h>
#include <cstring>

struct Hang { const xi_host_api* host; };

extern "C" __declspec(dllexport)
void* xi_plugin_create(const xi_host_api* host, const char*) { return new Hang{ host }; }

extern "C" __declspec(dllexport)
void xi_plugin_destroy(void* p) { delete static_cast<Hang*>(p); }

extern "C" __declspec(dllexport)
void xi_plugin_process(void*, const xi_record*, xi_record_out*) {
    // Block long enough to trigger the host's call timeout but bounded
    // so the worker eventually exits if the host never disconnects.
    Sleep(60000);
}

extern "C" __declspec(dllexport)
int xi_plugin_exchange(void*, const char*, char* rsp, int rsplen) {
    const char* m = "{\"who\":\"hang\"}";
    int n = (int)std::strlen(m);
    if (rsplen < n + 1) return -n;
    std::memcpy(rsp, m, n); rsp[n] = 0; return n;
}

extern "C" __declspec(dllexport)
int xi_plugin_get_def(void*, char* buf, int buflen) {
    if (buflen < 3) return -2; std::memcpy(buf, "{}", 3); return 2;
}

extern "C" __declspec(dllexport)
int xi_plugin_set_def(void*, const char*) { return 0; }
