//
// test_abi_freeze.cpp — the xi_host_api FREEZE GUARD (Phase 0 → evolved Phase 1).
//
// Declares the published xi_host_api layout FROZEN and fails the BUILD if it
// changes illegitimately. This is the freeze-signature guard from
// core_fix_plan.md §12 / ADR-001. The rule it encodes:
//
//     v11 is the NEW frozen baseline. Phase 4 deliberately BROKE the old v9-
//     prefix freeze (removed the dead shm_* block, retired xi.legacy, raised
//     min-compat to 11 — an authorized major break). From v11 onward a
//     published layout never changes in place — a new capability ships as the
//     next version or a carved interface behind get_interface.
//
// So this file pins TWO things at once:
//   (a) the 22 v11 fields — every OFFSET and exact fn-pointer TYPE, in order.
//       These are the v10 offsets MINUS the removed shm_* block (every field
//       after instance_folder shifted down 5 pointers / 40 bytes).
//   (b) the pins: get_interface at offset 168 (last field), version == 11,
//       size 176, min-compat 11.
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
// To evolve the ABI legitimately: do NOT edit the v11 table below to make a
// change pass. Carve a frozen per-capability interface (xi.preview@1, …) behind
// get_interface and pin IT with its own freeze table — the v11 baseline stays put.
//
#include <xi/xi_abi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

// ---- Version + size pins ------------------------------------------------
// v11 is the NEW frozen baseline (core_fix_plan.md §12 Phase 4): the dead shm_*
// block is gone and xi.legacy is retired — an authorized major break that
// invalidated the old v9-prefix pins. From here the freeze resumes: 22 fields,
// 176 bytes, min-compat raised to 11 (pre-v11 plugins refused).
static_assert(XI_ABI_VERSION == 11,
              "xi_host_api is at v11 (shm_* removed, xi.legacy retired — the new "
              "frozen baseline); a further change ships as a carved interface or "
              "the next version, not an in-place field edit (ADR-001).");
static_assert(XI_ABI_MIN_COMPAT == 11, "min-compat floor changed — v11 raised it to 11 (ADR-001).");
static_assert(XI_ABI_EXPECTED_SIZE == 176, "expected size changed — v11 is 176 (22 ptrs) (ADR-001).");
static_assert(sizeof(xi_host_api) == XI_ABI_EXPECTED_SIZE,
              "xi_host_api size changed — the v11 layout is frozen (ADR-001).");
static_assert(sizeof(void*) == 8, "freeze table assumes a 64-bit (8-byte ptr) host.");

// ---- Canonical v11 signature: per-field { offset, type } ----------------
// FROZEN — do not edit to make a layout change pass. These 22 fields ARE the v11
// baseline; a change ships as the next version or a carved interface, never an
// in-place edit. Offsets are the v10 layout MINUS the removed shm_* block (every
// field after instance_folder shifted down 5 pointers / 40 bytes). See header.
#define XI_FREEZE_FIELD(field, off, ...)                                        \
    static_assert(offsetof(xi_host_api, field) == (off),                        \
                  "xi_host_api::" #field " moved — the v11 layout is frozen");  \
    static_assert(std::is_same<decltype(xi_host_api::field), __VA_ARGS__>::value, \
                  "xi_host_api::" #field " retyped — the v11 signature is frozen")

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

// [ v11: the shm_* block (was 80..112 in v10) is REMOVED. read_image_file now
//   sits where shm_create_image used to, at offset 80. ]

// File I/O + status (80..88)
XI_FREEZE_FIELD(read_image_file, 80,  xi_image_handle (*)(const char*));
XI_FREEZE_FIELD(set_status,      88,  void            (*)(const char*, const char*));

// Doc allocator (ABI v3) + refcount (ABI v4) (96..136)
XI_FREEZE_FIELD(doc_chunk_alloc,   96,  void* (*)(size_t));
XI_FREEZE_FIELD(doc_chunk_realloc, 104, void* (*)(void*, size_t));
XI_FREEZE_FIELD(doc_chunk_free,    112, void  (*)(void*));
XI_FREEZE_FIELD(doc_retain,        120, void  (*)(void*));
XI_FREEZE_FIELD(doc_release,       128, void  (*)(void*));
XI_FREEZE_FIELD(doc_refcount,      136, int32_t (*)(void*));

// Dispatch + outputs (ABI v6/v8/v9) (144..160)
XI_FREEZE_FIELD(emit_record,    144, void    (*)(const char*, xi_trigger_id,
                                                 const struct xi_record*, int64_t));
XI_FREEZE_FIELD(emit_binary,    152, void    (*)(const void*, int32_t));
XI_FREEZE_FIELD(compress_image, 160, int32_t (*)(const void*, int32_t, int32_t,
                                                 int32_t, int32_t, void*, int32_t));

// ---- the capability-query door (core_fix_plan.md §12 Phase 1) ----------------
// get_interface remains the last field, now at offset 168 (= XI_ABI_EXPECTED_SIZE
// - sizeof(void*)) after the shm_* removal shifted everything down. compress_image
// is the last non-door field at 160. Future capabilities are carved as frozen
// interfaces behind this door, NOT appended here.
XI_FREEZE_FIELD(get_interface, 168, const void* (*)(const char*, uint32_t));
static_assert(offsetof(xi_host_api, get_interface) == 168,
              "get_interface must sit at offset 168 (the last field) in the v11 "
              "layout, right after compress_image @ 160.");
static_assert(offsetof(xi_host_api, compress_image) == 160,
              "the v11 layout moved: compress_image must remain the last non-door "
              "field at offset 160.");

#undef XI_FREEZE_FIELD

// ---- Carved capability interfaces — frozen per-interface signatures ---------
// ADDITIVE (does NOT touch the v9/v10 host-api pins above). Phase 3 carves the
// remaining domains out of the monolith into segregated, independently-frozen
// structs reached through get_interface. Each published (id, vN) is frozen
// FOREVER: a change ships as vN+1, never an in-place edit. These pins make any
// reorder/retype of a carved interface a COMPILE error — the same discipline the
// v9 prefix gets. (The "matches the legacy field byte-for-byte" runtime proof is
// in test_interface_domains.cpp; here we freeze the struct SHAPE.)
#define XI_FREEZE_IFACE(type, field, off, ...)                                   \
    static_assert(offsetof(type, field) == (off),                               \
                  #type "::" #field " moved — a published interface is frozen"); \
    static_assert(std::is_same<decltype(type::field), __VA_ARGS__>::value,      \
                  #type "::" #field " retyped — a published interface is frozen")

// xi.preview@1 (carved Phase 2) — single entry.
static_assert(sizeof(xi_preview_v1) == 1 * sizeof(void*), "xi_preview_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_preview_v1, compress, 0,
                int32_t (*)(const void*, int32_t, int32_t, int32_t, int32_t, void*, int32_t));

// xi.imaging@1 — image pool (8) + read_image_file (1) = 9 entries.
static_assert(sizeof(xi_imaging_v1) == 9 * sizeof(void*), "xi_imaging_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_imaging_v1, image_create,    0,  xi_image_handle (*)(int32_t, int32_t, int32_t));
XI_FREEZE_IFACE(xi_imaging_v1, image_addref,    8,  void            (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_release,   16, void            (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_data,      24, uint8_t*        (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_width,     32, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_height,    40, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_channels,  48, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_stride,    56, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, read_image_file, 64, xi_image_handle (*)(const char*));

// xi.imaging_rw@1 — read/write access discipline (ext review 02 I.4) = 2 entries.
static_assert(sizeof(xi_imaging_rw_v1) == 2 * sizeof(void*), "xi_imaging_rw_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_imaging_rw_v1, image_read,  0, const uint8_t* (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_rw_v1, image_write, 8, uint8_t*       (*)(xi_image_handle));

// xi.doc@1 — host doc allocator (3) + refcount (3) = 6 entries.
static_assert(sizeof(xi_doc_v1) == 6 * sizeof(void*), "xi_doc_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_doc_v1, doc_chunk_alloc,   0,  void*   (*)(size_t));
XI_FREEZE_IFACE(xi_doc_v1, doc_chunk_realloc, 8,  void*   (*)(void*, size_t));
XI_FREEZE_IFACE(xi_doc_v1, doc_chunk_free,    16, void    (*)(void*));
XI_FREEZE_IFACE(xi_doc_v1, doc_retain,        24, void    (*)(void*));
XI_FREEZE_IFACE(xi_doc_v1, doc_release,       32, void    (*)(void*));
XI_FREEZE_IFACE(xi_doc_v1, doc_refcount,      40, int32_t (*)(void*));

// xi.emit@1 — emit_record + emit_binary = 2 entries.
static_assert(sizeof(xi_emit_v1) == 2 * sizeof(void*), "xi_emit_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_emit_v1, emit_record, 0, void (*)(const char*, xi_trigger_id,
                                                     const struct xi_record*, int64_t));
XI_FREEZE_IFACE(xi_emit_v1, emit_binary, 8, void (*)(const void*, int32_t));

// xi.log@1 — log + set_status = 2 entries.
static_assert(sizeof(xi_log_v1) == 2 * sizeof(void*), "xi_log_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_log_v1, log,        0, void (*)(int32_t, const char*));
XI_FREEZE_IFACE(xi_log_v1, set_status, 8, void (*)(const char*, const char*));

#undef XI_FREEZE_IFACE

int main() {
    // Everything load-bearing is checked at compile time above; reaching here means
    // the frozen v11 layout is intact (get_interface the last field, shm_* gone).
    std::printf("[test] xi_host_api@%d freeze guard: %zu fields, %zu bytes "
                "(v11 baseline frozen; get_interface @ %zu) — FROZEN\n",
                XI_ABI_VERSION, sizeof(xi_host_api) / sizeof(void*), sizeof(xi_host_api),
                offsetof(xi_host_api, get_interface));
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
