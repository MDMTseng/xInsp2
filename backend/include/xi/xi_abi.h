/*
 * xi_abi.h — stable C ABI for xInsp2 plugin boundaries.
 *
 * This header defines the data types that cross DLL boundaries. All types
 * are plain C structs with fixed layout — no std::string, no std::map,
 * no vtables. Safe across MSVC versions, and potentially across compilers.
 *
 * C++ wrappers in xi_abi.hpp convert between these and xi::Record/xi::Image.
 *
 * Rules:
 *   - All strings are const char* (null-terminated, UTF-8)
 *   - All image data is a borrowed pointer (caller owns the memory)
 *   - The callee must copy anything it wants to keep
 */

#ifndef XI_ABI_H
#define XI_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A single named image passed across the boundary. */
typedef struct {
    const char*    key;        /* image name, e.g. "input", "roi" */
    const uint8_t* data;       /* pixel data, row-major, interleaved */
    int32_t        width;
    int32_t        height;
    int32_t        channels;   /* 1, 3, or 4 */
    int32_t        stride;     /* bytes per row (usually width * channels) */
} xi_image_ref;

/* A record passed across the boundary: named images + JSON data. */
typedef struct {
    const xi_image_ref* images;      /* array of image refs */
    int32_t             image_count;
    const char*         json_data;   /* null-terminated JSON string */
} xi_record_ref;

/* An output record — callee fills this. Images are allocated by callee,
 * freed by caller via xi_record_out_free(). */
typedef struct {
    xi_image_ref*  images;           /* callee-allocated array */
    int32_t        image_count;
    int32_t        image_capacity;
    char*          json_data;        /* callee-allocated, null-terminated */
} xi_record_out;

/* Helper: callee calls this to add an image to the output. The data is
 * COPIED into a new allocation owned by the output. */
static inline void xi_record_out_add_image(
    xi_record_out* out, const char* key,
    const uint8_t* data, int32_t w, int32_t h, int32_t ch)
{
    if (out->image_count >= out->image_capacity) {
        int32_t new_cap = out->image_capacity ? out->image_capacity * 2 : 4;
        xi_image_ref* new_arr = (xi_image_ref*)realloc(out->images, new_cap * sizeof(xi_image_ref));
        if (!new_arr) return;
        out->images = new_arr;
        out->image_capacity = new_cap;
    }
    int32_t nbytes = w * h * ch;
    uint8_t* copy = (uint8_t*)malloc(nbytes);
    if (!copy) return;
    memcpy(copy, data, nbytes);

    xi_image_ref* ref = &out->images[out->image_count++];
    ref->key      = _strdup(key);
    ref->data     = copy;
    ref->width    = w;
    ref->height   = h;
    ref->channels = ch;
    ref->stride   = w * ch;
}

/* Helper: set the JSON data on an output record. String is copied. */
static inline void xi_record_out_set_json(xi_record_out* out, const char* json) {
    free(out->json_data);
    out->json_data = _strdup(json);
}

/* Free all allocations in an output record. Called by the caller (backend)
 * after consuming the data. */
static inline void xi_record_out_free(xi_record_out* out) {
    for (int32_t i = 0; i < out->image_count; ++i) {
        free((void*)out->images[i].key);
        free((void*)out->images[i].data);
    }
    free(out->images);
    free(out->json_data);
    out->images = NULL;
    out->image_count = 0;
    out->image_capacity = 0;
    out->json_data = NULL;
}

/* Initialize an output record to zeros. */
static inline void xi_record_out_init(xi_record_out* out) {
    out->images = NULL;
    out->image_count = 0;
    out->image_capacity = 0;
    out->json_data = NULL;
}

/*
 * Plugin factory and process functions — the stable ABI entry points.
 *
 * A plugin DLL exports:
 *
 *   void* xi_plugin_create(const char* instance_name);
 *   void  xi_plugin_destroy(void* instance);
 *   void  xi_plugin_process(void* instance,
 *                           const xi_record_ref* input,
 *                           xi_record_out* output);
 *   int   xi_plugin_exchange(void* instance,
 *                            const char* cmd_json,
 *                            char* rsp_buf, int rsp_buflen);
 *   int   xi_plugin_get_def(void* instance, char* buf, int buflen);
 *   int   xi_plugin_set_def(void* instance, const char* json);
 */

#ifdef __cplusplus
}
#endif

#endif /* XI_ABI_H */
