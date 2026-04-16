/*
 * xi_abi.h — the universal plugin interface for xInsp2.
 *
 * ALL plugins use this interface — built-in, third-party, native scripts.
 * Pure C types at the boundary. Stable across compiler versions.
 *
 * Key concept: images are refcounted HANDLES managed by the host.
 * Plugins never malloc/free image buffers — they ask the host to create
 * images, get a handle back, read/write via the host API, and release
 * when done. Caching = addref. Passing between plugins = zero copy
 * (just pass the handle).
 *
 * Flow:
 *
 *   1. Backend loads plugin DLL
 *   2. Calls xi_plugin_create(host_api, name) → opaque instance
 *   3. Calls xi_plugin_process(inst, input_record, output_record)
 *      - input images are handles the backend owns
 *      - plugin reads via host->image_data(handle)
 *      - plugin creates output images via host->image_create()
 *      - plugin puts output handles in the output record
 *      - to cache an input image: host->image_addref(handle)
 *   4. Backend reads output record, takes ownership of output handles
 *   5. When done: host->image_release() on all handles
 */

#ifndef XI_ABI_H
#define XI_ABI_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Image handle — opaque reference to a refcounted image in the host  */
/* ------------------------------------------------------------------ */

typedef uint32_t xi_image_handle;
#define XI_IMAGE_NULL 0

/* ------------------------------------------------------------------ */
/* Host API — function table provided by the backend to every plugin  */
/* ------------------------------------------------------------------ */

typedef struct xi_host_api {
    /* Image pool */
    xi_image_handle (*image_create)(int32_t w, int32_t h, int32_t channels);
    void            (*image_addref)(xi_image_handle h);
    void            (*image_release)(xi_image_handle h);
    uint8_t*        (*image_data)(xi_image_handle h);
    int32_t         (*image_width)(xi_image_handle h);
    int32_t         (*image_height)(xi_image_handle h);
    int32_t         (*image_channels)(xi_image_handle h);
    int32_t         (*image_stride)(xi_image_handle h);

    /* Logging */
    void (*log)(int32_t level, const char* msg);
    /* level: 0=debug, 1=info, 2=warn, 3=error */
} xi_host_api;

/* ------------------------------------------------------------------ */
/* Record — the universal data container crossing the boundary        */
/* ------------------------------------------------------------------ */

/* A named image entry in a record. */
typedef struct {
    const char*      key;       /* borrowed — valid for duration of the call */
    xi_image_handle  handle;
} xi_record_image;

/* A record: named images + JSON metadata. */
typedef struct {
    const xi_record_image* images;
    int32_t                image_count;
    const char*            json;    /* null-terminated JSON object string */
} xi_record;

/* Output record — plugin fills this during process(). */
typedef struct {
    xi_record_image* images;
    int32_t          image_count;
    int32_t          image_capacity;
    char*            json;          /* plugin sets this (malloc'd, caller frees) */
} xi_record_out;

/* Helpers for building output records */

static inline void xi_record_out_init(xi_record_out* out) {
    out->images = NULL;
    out->image_count = 0;
    out->image_capacity = 0;
    out->json = NULL;
}

static inline void xi_record_out_add_image(xi_record_out* out,
                                            const char* key,
                                            xi_image_handle handle) {
    if (out->image_count >= out->image_capacity) {
        int32_t new_cap = out->image_capacity ? out->image_capacity * 2 : 8;
        xi_record_image* arr = (xi_record_image*)realloc(
            out->images, (size_t)new_cap * sizeof(xi_record_image));
        if (!arr) return;
        out->images = arr;
        out->image_capacity = new_cap;
    }
    xi_record_image* entry = &out->images[out->image_count++];
    entry->key = _strdup(key);
    entry->handle = handle;
}

static inline void xi_record_out_set_json(xi_record_out* out, const char* json) {
    free(out->json);
    out->json = _strdup(json);
}

static inline void xi_record_out_free(xi_record_out* out) {
    for (int32_t i = 0; i < out->image_count; ++i) {
        free((void*)out->images[i].key);
        /* NOTE: do NOT release the image handle here — the backend
         * takes ownership of output handles. */
    }
    free(out->images);
    free(out->json);
    out->images = NULL;
    out->image_count = 0;
    out->image_capacity = 0;
    out->json = NULL;
}

/* ------------------------------------------------------------------ */
/* Plugin entry points — exported by every plugin DLL                 */
/* ------------------------------------------------------------------ */

/*
 * xi_plugin_create(host, name) → opaque instance pointer
 *   Called once per instance. Plugin stores the host_api pointer for
 *   later use (image_create, image_data, etc.)
 *
 * xi_plugin_destroy(inst)
 *   Release all cached handles and free the instance.
 *
 * xi_plugin_process(inst, input, output)
 *   Run the plugin's core logic. Read input images via host->image_data().
 *   Create output images via host->image_create(). Put handles in output.
 *
 * xi_plugin_exchange(inst, cmd_json, rsp_buf, rsp_buflen) → bytes written
 *   UI/config commands. Returns JSON response.
 *
 * xi_plugin_get_def(inst, buf, buflen) → bytes written
 *   Serialize current config to JSON.
 *
 * xi_plugin_set_def(inst, json) → 0 on success
 *   Restore config from JSON.
 */

/* Type signatures for GetProcAddress */
typedef void* (*xi_plugin_create_fn)(const xi_host_api* host, const char* name);
typedef void  (*xi_plugin_destroy_fn)(void* inst);
typedef void  (*xi_plugin_process_fn)(void* inst, const xi_record* input, xi_record_out* output);
typedef int   (*xi_plugin_exchange_fn)(void* inst, const char* cmd, char* rsp, int rsplen);
typedef int   (*xi_plugin_get_def_fn)(void* inst, char* buf, int buflen);
typedef int   (*xi_plugin_set_def_fn)(void* inst, const char* json);

#ifdef __cplusplus
}
#endif

#endif /* XI_ABI_H */
