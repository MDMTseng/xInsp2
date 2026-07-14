//
// test_abi_freeze.cpp — the xi_host_api FREEZE GUARD (Phase 0 → evolved Phase 1).
//
// Declares the published xi_host_api layout FROZEN and fails the BUILD if it
// changes illegitimately. This is the freeze-signature guard from
// core_fix_plan.md §12 / ADR-001. The rule it encodes:
//
//     v12 is the NEW frozen baseline. THE CUT deliberately BROKE the v11 freeze
//     (the Record data plane was deleted: emit_record + the doc allocator/
//     refcount slots removed, read_image_file evicted to xi.image.decode, and
//     min-compat raised to 12 — an authorized major break). From v12 onward a
//     published layout never changes in place — a new capability ships as the
//     next version or a carved interface behind get_interface.
//
// So this file pins TWO things at once:
//   (a) the 14 v12 fields — every OFFSET and exact fn-pointer TYPE, in order.
//       These are the v11 fields MINUS read_image_file, the six doc slots, and
//       emit_record (the Record yyjson-doc dispatch path). Every field after
//       instance_folder shifted UP accordingly.
//   (b) the pins: get_interface at offset 104 (last field), version == 12,
//       size 112, min-compat 12.
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
// To evolve the ABI legitimately: do NOT edit the v12 table below to make a
// change pass. Carve a frozen per-capability interface (xi.preview@1, …) behind
// get_interface and pin IT with its own freeze table — the v12 baseline stays put.
//
#include <xi/xi_abi.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

// ---- Version + size pins ------------------------------------------------
// v12 is the NEW frozen baseline (THE CUT): the Record data plane is deleted —
// emit_record + doc_chunk_*/doc_retain/release/refcount + read_image_file are
// gone (14 fields, 112 bytes), and min-compat raised to 12 (pre-v12 plugins
// refused). The plugin data plane is the xi.pack@1 door.
static_assert(XI_ABI_VERSION == 12,
              "xi_host_api is at v12 (THE CUT: Record data plane deleted — the new "
              "frozen baseline); a further change ships as a carved interface or "
              "the next version, not an in-place field edit (ADR-001).");
static_assert(XI_ABI_MIN_COMPAT == 12, "min-compat floor changed — THE CUT raised it to 12 (ADR-001).");
static_assert(XI_ABI_EXPECTED_SIZE == 112, "expected size changed — v12 is 112 (14 ptrs) (ADR-001).");
static_assert(sizeof(xi_host_api) == XI_ABI_EXPECTED_SIZE,
              "xi_host_api size changed — the v12 layout is frozen (ADR-001).");
static_assert(sizeof(void*) == 8, "freeze table assumes a 64-bit (8-byte ptr) host.");

// ---- Canonical v12 signature: per-field { offset, type } ----------------
// FROZEN — do not edit to make a layout change pass. These 14 fields ARE the v12
// baseline; a change ships as the next version or a carved interface, never an
// in-place edit.
#define XI_FREEZE_FIELD(field, off, ...)                                        \
    static_assert(offsetof(xi_host_api, field) == (off),                        \
                  "xi_host_api::" #field " moved — the v12 layout is frozen");  \
    static_assert(std::is_same<decltype(xi_host_api::field), __VA_ARGS__>::value, \
                  "xi_host_api::" #field " retyped — the v12 signature is frozen")

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

// [ THE CUT (v12): read_image_file (was @80), the six doc slots (was @96..136),
//   and emit_record (was @144) are DELETED. set_status/emit_binary/compress_image
//   shift up to fill the gap. ]

// Status + WS binary push + JPEG encode (80..96)
XI_FREEZE_FIELD(set_status,     80,  void            (*)(const char*, const char*));
XI_FREEZE_FIELD(emit_binary,    88,  void    (*)(const void*, int32_t));
XI_FREEZE_FIELD(compress_image, 96,  int32_t (*)(const void*, int32_t, int32_t,
                                                 int32_t, int32_t, void*, int32_t));

// ---- the capability-query door (core_fix_plan.md §12 Phase 1) ----------------
// get_interface remains the last field, now at offset 104 (= XI_ABI_EXPECTED_SIZE
// - sizeof(void*)). compress_image is the last non-door field at 96. Future
// capabilities are carved as frozen interfaces behind this door, NOT appended here.
XI_FREEZE_FIELD(get_interface, 104, const void* (*)(const char*, uint32_t));
static_assert(offsetof(xi_host_api, get_interface) == 104,
              "get_interface must sit at offset 104 (the last field) in the v12 "
              "layout, right after compress_image @ 96.");
static_assert(offsetof(xi_host_api, compress_image) == 96,
              "the v12 layout moved: compress_image must remain the last non-door "
              "field at offset 96.");

#undef XI_FREEZE_FIELD

// ---- Carved capability interfaces — frozen per-interface signatures ---------
// ADDITIVE (does NOT touch the host-api pins above). Each published (id, vN) is
// frozen FOREVER: a change ships as vN+1, never an in-place edit. These pins make
// any reorder/retype of a carved interface a COMPILE error.
#define XI_FREEZE_IFACE(type, field, off, ...)                                   \
    static_assert(offsetof(type, field) == (off),                               \
                  #type "::" #field " moved — a published interface is frozen"); \
    static_assert(std::is_same<decltype(type::field), __VA_ARGS__>::value,      \
                  #type "::" #field " retyped — a published interface is frozen")

// xi.preview@1 (carved Phase 2) — single entry.
static_assert(sizeof(xi_preview_v1) == 1 * sizeof(void*), "xi_preview_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_preview_v1, compress, 0,
                int32_t (*)(const void*, int32_t, int32_t, int32_t, int32_t, void*, int32_t));

// xi.imaging@1 — image pool (8) = 8 entries. [THE CUT: read_image_file dropped.]
static_assert(sizeof(xi_imaging_v1) == 8 * sizeof(void*), "xi_imaging_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_imaging_v1, image_create,    0,  xi_image_handle (*)(int32_t, int32_t, int32_t));
XI_FREEZE_IFACE(xi_imaging_v1, image_addref,    8,  void            (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_release,   16, void            (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_data,      24, uint8_t*        (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_width,     32, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_height,    40, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_channels,  48, int32_t         (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_v1, image_stride,    56, int32_t         (*)(xi_image_handle));

// xi.imaging_rw@1 — read/write access discipline (ext review 02 I.4) = 2 entries.
static_assert(sizeof(xi_imaging_rw_v1) == 2 * sizeof(void*), "xi_imaging_rw_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_imaging_rw_v1, image_read,  0, const uint8_t* (*)(xi_image_handle));
XI_FREEZE_IFACE(xi_imaging_rw_v1, image_write, 8, uint8_t*       (*)(xi_image_handle));

// [ xi.doc@1 — host doc allocator + refcount — was DELETED at THE CUT with the
//   Record yyjson-doc plane; its freeze block is gone. ]

// xi.emit@1 — emit_binary = 1 entry. [THE CUT: emit_record dropped; the Record
// source-emit verb is replaced by xi.pack@1 emit_pack.]
static_assert(sizeof(xi_emit_v1) == 1 * sizeof(void*), "xi_emit_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_emit_v1, emit_binary, 0, void (*)(const void*, int32_t));

// xi.log@1 — log + set_status = 2 entries.
static_assert(sizeof(xi_log_v1) == 2 * sizeof(void*), "xi_log_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_log_v1, log,        0, void (*)(int32_t, const char*));
XI_FREEZE_IFACE(xi_log_v1, set_status, 8, void (*)(const char*, const char*));

// xi.pack@1 (HOST door) — the v3-slab data plane's frozen keyed-buffer vtable =
// 25 entries. FROZEN FOREVER: the builder_add_bool/get_bool tail was the last
// pre-cutover append; all later growth ships as xi.pack@3 below. The header
// carries a size + last-field static_assert too (belt); this pins every field.
static_assert(sizeof(xi_pack_v1) == 25 * sizeof(void*), "xi_pack_v1 size changed (frozen @1)");
XI_FREEZE_IFACE(xi_pack_v1, builder_new,         0,   xi_pack_builder (*)(void));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_i64,     8,   void (*)(xi_pack_builder, const char*, int64_t));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_f64,     16,  void (*)(xi_pack_builder, const char*, double));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_str,     24,  void (*)(xi_pack_builder, const char*, const char*, int32_t));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_bin,     32,  void (*)(xi_pack_builder, const char*, const void*, int32_t));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_image,   40,  void (*)(xi_pack_builder, const char*, int32_t, int32_t, int32_t, const void*));
XI_FREEZE_IFACE(xi_pack_v1, builder_adopt_image, 48,  void (*)(xi_pack_builder, const char*, int32_t, int32_t, int32_t, xi_image_handle));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_mp,      56,  void (*)(xi_pack_builder, const char*, const void*, int32_t));
XI_FREEZE_IFACE(xi_pack_v1, builder_seal,        64,  xi_pack_handle (*)(xi_pack_builder));
XI_FREEZE_IFACE(xi_pack_v1, builder_abandon,     72,  void (*)(xi_pack_builder));
XI_FREEZE_IFACE(xi_pack_v1, count,               80,  int32_t (*)(xi_pack_handle));
XI_FREEZE_IFACE(xi_pack_v1, key_at,              88,  const char* (*)(xi_pack_handle, int32_t, int32_t*));
XI_FREEZE_IFACE(xi_pack_v1, tag_at,              96,  int32_t (*)(xi_pack_handle, int32_t));
XI_FREEZE_IFACE(xi_pack_v1, tag_of,              104, int32_t (*)(xi_pack_handle, const char*));
XI_FREEZE_IFACE(xi_pack_v1, get_i64,             112, int32_t (*)(xi_pack_handle, const char*, int64_t*));
XI_FREEZE_IFACE(xi_pack_v1, get_f64,             120, int32_t (*)(xi_pack_handle, const char*, double*));
XI_FREEZE_IFACE(xi_pack_v1, get_str,             128, int32_t (*)(xi_pack_handle, const char*, const char**, int32_t*));
XI_FREEZE_IFACE(xi_pack_v1, get_bin,             136, int32_t (*)(xi_pack_handle, const char*, const void**, int32_t*));
XI_FREEZE_IFACE(xi_pack_v1, get_image,           144, int32_t (*)(xi_pack_handle, const char*, xi_pack_image*));
XI_FREEZE_IFACE(xi_pack_v1, get_mp,              152, int32_t (*)(xi_pack_handle, const char*, const void**, int32_t*));
XI_FREEZE_IFACE(xi_pack_v1, retain,              160, void (*)(xi_pack_handle));
XI_FREEZE_IFACE(xi_pack_v1, release,             168, void (*)(xi_pack_handle));
XI_FREEZE_IFACE(xi_pack_v1, emit_pack,           176, void (*)(const char*, xi_trigger_id, xi_pack_handle, int64_t));
XI_FREEZE_IFACE(xi_pack_v1, builder_add_bool,    184, void (*)(xi_pack_builder, const char*, int32_t));
XI_FREEZE_IFACE(xi_pack_v1, get_bool,            192, int32_t (*)(xi_pack_handle, const char*, int32_t*));

// xi.pack@3 (HOST door) — the slab-generation supplement (tensors, typed blobs,
// ordinal walk) = 9 entries. FROZEN: a change ships as xi.pack@N+1, never in place.
static_assert(sizeof(xi_pack_v3) == 9 * sizeof(void*), "xi_pack_v3 size changed (frozen @3)");
XI_FREEZE_IFACE(xi_pack_v3, builder_add_tensor,   0,  int32_t (*)(xi_pack_builder, const char*, int32_t, int32_t, int32_t, int32_t, const void*));
XI_FREEZE_IFACE(xi_pack_v3, builder_adopt_tensor, 8,  int32_t (*)(xi_pack_builder, const char*, int32_t, int32_t, int32_t, int32_t, xi_image_handle));
XI_FREEZE_IFACE(xi_pack_v3, builder_add_blob,     16, int32_t (*)(xi_pack_builder, const char*, int32_t, const void*, int32_t));
XI_FREEZE_IFACE(xi_pack_v3, builder_adopt_bin,    24, int32_t (*)(xi_pack_builder, const char*, int32_t, xi_image_handle));
XI_FREEZE_IFACE(xi_pack_v3, get_tensor,           32, int32_t (*)(xi_pack_handle, const char*, xi_pack_tensor*));
XI_FREEZE_IFACE(xi_pack_v3, get_blob,             40, int32_t (*)(xi_pack_handle, const char*, int32_t*, const void**, int32_t*));
XI_FREEZE_IFACE(xi_pack_v3, type_id_of,           48, int32_t (*)(xi_pack_handle, const char*));
XI_FREEZE_IFACE(xi_pack_v3, type_id_at,           56, int32_t (*)(xi_pack_handle, int32_t));
XI_FREEZE_IFACE(xi_pack_v3, entry_at,             64, int32_t (*)(xi_pack_handle, int32_t, xi_pack_entry*));

#undef XI_FREEZE_IFACE

int main() {
    // Everything load-bearing is checked at compile time above; reaching here means
    // the frozen v12 layout is intact (get_interface the last field; Record plane gone).
    std::printf("[test] xi_host_api@%d freeze guard: %zu fields, %zu bytes "
                "(v12 baseline frozen; get_interface @ %zu) — FROZEN\n",
                XI_ABI_VERSION, sizeof(xi_host_api) / sizeof(void*), sizeof(xi_host_api),
                offsetof(xi_host_api, get_interface));
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
