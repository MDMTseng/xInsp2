//
// test_abi_freeze.cpp — the xi_host_api@9 FREEZE GUARD (Phase 0).
//
// Declares `xi_host_api` version 9 FROZEN and fails the BUILD if its published
// layout changes. This is the freeze-signature guard from core_fix_plan.md §12 /
// ADR-001: a published (interface, vN) is frozen forever; any change ships as
// vN+1. The seed was the lone `offsetof(compress_image)` assert in xi_abi.h; this
// strengthens it into a per-field canonical signature — every field's OFFSET and
// its exact function-pointer TYPE (return + parameters), in order.
//
// Why this and not just sizeof: sizeof catches a size delta, but a same-size
// reorder or a retype (e.g. swapping two void(*)(...) fields, or changing an
// int32_t param to int64_t on a like-sized pointer) slips past it. Pinning every
// field's offset AND type makes ANY observable layout/contract change a compile
// error here.
//
// Almost all the work is at COMPILE TIME (static_assert), so this fires during
// the normal test build — there is no CI runner in this repo, so the freeze guard
// IS this test. main() is a thin runtime confirmation for ctest.
//
// To change the ABI: do NOT edit the table below to make it pass. Append a new
// field (Phase 1 get_interface), bump XI_ABI_VERSION → 10, update
// XI_ABI_EXPECTED_SIZE, and add a SECOND frozen table for v10 — leaving this v9
// table intact as the permanent record of the frozen v9 signature.
//
#include <xi/xi_abi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

// ---- Version + size pins ------------------------------------------------
static_assert(XI_ABI_VERSION == 9,
              "xi_host_api is frozen at v9; a change must ship as v10 (ADR-001).");
static_assert(XI_ABI_MIN_COMPAT == 6, "min-compat floor changed (ADR-001).");
static_assert(XI_ABI_EXPECTED_SIZE == 208, "expected size changed (ADR-001).");
static_assert(sizeof(xi_host_api) == XI_ABI_EXPECTED_SIZE,
              "xi_host_api size changed — the v9 layout is frozen (ADR-001).");
static_assert(sizeof(void*) == 8, "freeze table assumes a 64-bit (8-byte ptr) host.");

// ---- Canonical v9 signature: per-field { offset, type } -----------------
// FROZEN — do not edit to make a layout change pass. See the file header.
#define XI_FREEZE_FIELD(field, off, ...)                                        \
    static_assert(offsetof(xi_host_api, field) == (off),                        \
                  "xi_host_api::" #field " moved — v9 layout is frozen");       \
    static_assert(std::is_same<decltype(xi_host_api::field), __VA_ARGS__>::value, \
                  "xi_host_api::" #field " retyped — v9 signature is frozen")

// Image pool (offsets 0..56)
XI_FREEZE_FIELD(image_create,    0,   xi_image_handle (*)(int32_t, int32_t, int32_t));
XI_FREEZE_FIELD(image_addref,    8,   void            (*)(xi_image_handle));
XI_FREEZE_FIELD(image_release,   16,  void            (*)(xi_image_handle));
XI_FREEZE_FIELD(image_data,      24,  uint8_t*        (*)(xi_image_handle));
XI_FREEZE_FIELD(image_width,     32,  int32_t         (*)(xi_image_handle));
XI_FREEZE_FIELD(image_height,    40,  int32_t         (*)(xi_image_handle));
XI_FREEZE_FIELD(image_channels,  48,  int32_t         (*)(xi_image_handle));
XI_FREEZE_FIELD(image_stride,    56,  int32_t         (*)(xi_image_handle));

// Logging + instance folder (64..72)
XI_FREEZE_FIELD(log,             64,  void            (*)(int32_t, const char*));
XI_FREEZE_FIELD(instance_folder, 72,  int32_t         (*)(const char*, char*, int32_t));

// SHM stubs — retained NULL for layout stability (80..112)
XI_FREEZE_FIELD(shm_create_image,  80,  xi_image_handle (*)(int32_t, int32_t, int32_t));
XI_FREEZE_FIELD(shm_alloc_buffer,  88,  xi_image_handle (*)(int32_t));
XI_FREEZE_FIELD(shm_addref,        96,  void            (*)(xi_image_handle));
XI_FREEZE_FIELD(shm_release,       104, void            (*)(xi_image_handle));
XI_FREEZE_FIELD(shm_is_shm_handle, 112, int32_t         (*)(xi_image_handle));

// File I/O + status (120..128)
XI_FREEZE_FIELD(read_image_file, 120, xi_image_handle (*)(const char*));
XI_FREEZE_FIELD(set_status,      128, void            (*)(const char*, const char*));

// Doc allocator (ABI v3) + refcount (ABI v4) (136..176)
XI_FREEZE_FIELD(doc_chunk_alloc,   136, void* (*)(size_t));
XI_FREEZE_FIELD(doc_chunk_realloc, 144, void* (*)(void*, size_t));
XI_FREEZE_FIELD(doc_chunk_free,    152, void  (*)(void*));
XI_FREEZE_FIELD(doc_retain,        160, void  (*)(void*));
XI_FREEZE_FIELD(doc_release,       168, void  (*)(void*));
XI_FREEZE_FIELD(doc_refcount,      176, int32_t (*)(void*));

// Dispatch + outputs (ABI v6/v8/v9) (184..200)
XI_FREEZE_FIELD(emit_record,    184, void    (*)(const char*, xi_trigger_id,
                                                 const struct xi_record*, int64_t));
XI_FREEZE_FIELD(emit_binary,    192, void    (*)(const void*, int32_t));
XI_FREEZE_FIELD(compress_image, 200, int32_t (*)(const void*, int32_t, int32_t,
                                                 int32_t, int32_t, void*, int32_t));

#undef XI_FREEZE_FIELD

int main() {
    // Everything load-bearing is checked at compile time above; reaching here means
    // the frozen v9 signature is intact.
    std::printf("[test] xi_host_api@%d freeze guard: %zu fields, %zu bytes — FROZEN\n",
                XI_ABI_VERSION, sizeof(xi_host_api) / sizeof(void*), sizeof(xi_host_api));
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
