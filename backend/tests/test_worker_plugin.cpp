//
// test_worker_plugin.cpp — minimal plugin for the worker E2E test.
//
// On process(): reads the input image (whatever the backend handed us
// via SHM handle), allocates an output image in the SHM region via
// host->shm_create_image, and writes 2x each input pixel into it.
//
// Validates the zero-copy contract end-to-end:
//   backend writes input bytes → worker process reads same bytes via
//   SHM → worker writes output bytes via shm_create_image → backend
//   reads output via the SAME handle worker returned, in shared memory.
//

#include <xi/xi_abi.h>
#include <cstdint>
#include <cstring>

struct Doubler {
    const xi_host_api* host;
};

extern "C" __declspec(dllexport)
void* xi_plugin_create(const xi_host_api* host, const char* /*name*/) {
    auto* p = new Doubler{ host };
    return p;
}

extern "C" __declspec(dllexport)
void xi_plugin_destroy(void* p) { delete static_cast<Doubler*>(p); }

extern "C" __declspec(dllexport)
void xi_plugin_process(void* p_, const xi_record* in, xi_record_out* out) {
    auto* p = static_cast<Doubler*>(p_);
    if (!in || in->image_count <= 0) return;

    xi_image_handle in_h = in->images[0].handle;
    int w  = p->host->image_width(in_h);
    int h  = p->host->image_height(in_h);
    int ch = p->host->image_channels(in_h);
    if (w <= 0 || h <= 0 || ch <= 0) return;

    // Allocate the output in SHM so the BACKEND (a different process)
    // can see it without a copy.
    xi_image_handle out_h = p->host->shm_create_image(w, h, ch);
    if (!out_h) return;

    const uint8_t* src = p->host->image_data(in_h);
    uint8_t*       dst = p->host->image_data(out_h);
    const int total = w * h * ch;
    for (int i = 0; i < total; ++i) {
        // Saturating x2 — gives a deterministic pattern the test verifies.
        int v = src[i] * 2;
        dst[i] = (uint8_t)(v > 255 ? 255 : v);
    }

    // The plugin owns the output handle's ref now; hand it to the
    // worker's harness via xi_record_out so it can ship to backend.
    static xi_record_image out_img;  // single-image out, harness copies seq before next call
    out_img.key    = "doubled";
    out_img.handle = out_h;
    out->images       = &out_img;
    out->image_count  = 1;
    out->json         = nullptr;
}

extern "C" __declspec(dllexport)
int xi_plugin_exchange(void* /*p*/, const char* /*cmd*/, char* rsp, int rsplen) {
    const char* msg = "{\"plugin\":\"doubler\"}";
    int n = (int)std::strlen(msg);
    if (rsplen < n + 1) return -n;
    std::memcpy(rsp, msg, n); rsp[n] = 0;
    return n;
}

extern "C" __declspec(dllexport)
int xi_plugin_get_def(void* /*p*/, char* buf, int buflen) {
    const char* def = "{}";
    int n = (int)std::strlen(def);
    if (buflen < n + 1) return -n;
    std::memcpy(buf, def, n); buf[n] = 0;
    return n;
}

extern "C" __declspec(dllexport)
int xi_plugin_set_def(void* /*p*/, const char* /*json*/) { return 0; }
