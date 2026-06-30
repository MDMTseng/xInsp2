//
// test_abi_freeze.cpp — the xi_host_api FREEZE GUARD (Phase 0 → evolved Phase 1).
//
// Declares the published xi_host_api layout FROZEN and fails the BUILD if it
// changes illegitimately. This is the freeze-signature guard from
// core_fix_plan.md §12 / ADR-001. The rule it encodes:
//
//     The v9 fields are frozen FOREVER; v10 == the v9 prefix + one appended
//     pointer (get_interface). A published (interface, vN) never changes
//     in place — a new capability ships as the next version.
//
// So this file pins TWO things at once:
//   (a) the 26 v9 fields — every OFFSET and exact fn-pointer TYPE, in order,
//       UNCHANGED from the Phase-0 table. This proves the v9 prefix never
//       moved when v10 appended get_interface (an old v9 plugin's view of the
//       table is byte-identical).
//   (b) the v10 append — get_interface at offset 208, version == 10, size 216.
//
// Why per-field and not just sizeof: sizeof catches a size delta, but a same-
// size reorder or retype (swapping two void(*)(...) fields, or widening an
// int32_t param to int64_t on a like-sized pointer) slips past it. Pinning
// every field's offset AND type makes ANY observable change a compile error.
//
// Almost all the work is at COMPILE TIME (static_assert), so this fires during
// the normal test build — there is no CI runner in this repo, so the freeze
// guard IS this test. main() is a thin runtime confirmation for ctest.
//
// To evolve the ABI legitimately: do NOT edit the v9 table below to make a
// change pass. Carve a frozen per-capability interface (xi.preview@1, …) behind
// get_interface and pin IT with its own freeze table — the v9 prefix stays put.
//
#include <xi/xi_abi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

// ---- Version + size pins ------------------------------------------------
// v10 = v9 prefix + one appended pointer (get_interface). The v9 prefix is
// frozen forever; min-compat stays 6 (old plugins still load — the append is
// invisible to them).
static_assert(XI_ABI_VERSION == 10,
              "xi_host_api is at v10 (v9 + appended get_interface); a further "
              "change ships as a carved interface, not a field (ADR-001).");
static_assert(XI_ABI_MIN_COMPAT == 6, "min-compat floor changed (ADR-001).");
static_assert(XI_ABI_EXPECTED_SIZE == 216, "expected size changed (ADR-001).");
static_assert(sizeof(xi_host_api) == XI_ABI_EXPECTED_SIZE,
              "xi_host_api size changed — only the v10 get_interface append is "
              "legal beyond the frozen v9 prefix (ADR-001).");
static_assert(sizeof(void*) == 8, "freeze table assumes a 64-bit (8-byte ptr) host.");

// ---- Canonical v9 signature: per-field { offset, type } -----------------
// FROZEN — do not edit to make a layout change pass. These 26 fields are the v9
// prefix and must keep their EXACT offsets + types under v10 (the append goes
// AFTER them). See the file header.
#define XI_FREEZE_FIELD(field, off, ...)                                        \
    static_assert(offsetof(xi_host_api, field) == (off),                        \
                  "xi_host_api::" #field " moved — v9 prefix is frozen");       \
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

// ---- v10 append: the capability-query door (core_fix_plan.md §12 Phase 1) ----
// The ONE legal monolith append. It sits AFTER the frozen v9 prefix at offset
// 208 (= XI_ABI_EXPECTED_SIZE - sizeof(void*)); compress_image stays the last v9
// field at 200. Future capabilities are carved as frozen interfaces behind this
// door, NOT appended here.
XI_FREEZE_FIELD(get_interface, 208, const void* (*)(const char*, uint32_t));
static_assert(offsetof(xi_host_api, get_interface) == 208,
              "get_interface (v10 append) must sit at offset 208, right after the "
              "frozen v9 prefix (compress_image @ 200).");
static_assert(offsetof(xi_host_api, compress_image) == 200,
              "the v9 prefix moved: compress_image must remain the LAST v9 field "
              "at offset 200 under the v10 append.");

#undef XI_FREEZE_FIELD

int main() {
    // Everything load-bearing is checked at compile time above; reaching here means
    // the frozen v9 prefix is intact AND get_interface is the lone v10 append.
    std::printf("[test] xi_host_api@%d freeze guard: %zu fields, %zu bytes "
                "(v9 prefix frozen + get_interface @ %zu) — FROZEN\n",
                XI_ABI_VERSION, sizeof(xi_host_api) / sizeof(void*), sizeof(xi_host_api),
                offsetof(xi_host_api, get_interface));
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
