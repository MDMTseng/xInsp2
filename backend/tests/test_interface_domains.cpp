//
// test_interface_domains.cpp — Phase 3 proof: the SURVIVING capability domains
// (xi.imaging@1 / xi.imaging_rw@1 / xi.emit@1 [emit_binary only] / xi.log@1) are
// carved out of the v9 monolith as frozen, segregated interfaces reached through
// host->get_interface, each one byte-for-byte IDENTICAL to the legacy xi_host_api
// fields it groups.
//
// THE CUT (v12) retired several planes that this test used to cover: the
// xi.doc@1 interface (doc_chunk_*/doc_retain/release/refcount), the emit_record
// slot on xi.emit@1, and the read_image_file host slot (evicted to the
// xi.image.decode capability, dropped from xi.imaging@1). Every section/assert
// that referenced those DELETED planes has been removed; only the live-domain
// coverage remains.
//
// core_fix_plan.md §12 Phase 3. The carve is PURELY ADDITIVE: it does not move
// any xi_host_api field (the freeze guard in test_abi_freeze.cpp stays green).
// An interface just exposes a SUBSET of the same function pointers through the
// query door, so:
//   * a caller reaching a capability via the door gets the SAME fn pointer as
//     one calling the legacy field — proven here by direct pointer equality;
//   * the SDK xi::Plugin wrappers resolve-then-cache the interface with a
//     legacy-field fallback, so a (simulated) pre-v10 host keeps working.
//
#include <xi/xi_abi.h>
#include <xi/xi_abi.hpp>          // xi::Plugin (SDK wrappers)
#include <xi/xi_image_pool.hpp>   // xi::ImagePool::make_host_api
#include <xi/xi_binary_sink.hpp>
#include <xi/xi_status_sink.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Observers for the emit/log domains so the SDK wrappers can be seen to forward.
static int     g_binary_calls = 0;
static int     g_binary_len   = 0;
static void    obs_binary(const void* /*d*/, int len) { ++g_binary_calls; g_binary_len = len; }
static std::string g_status_src, g_status_txt;
static void    obs_status(const char* s, const char* t) {
    g_status_src = s ? s : ""; g_status_txt = t ? t : "";
}

int main() {
    std::printf("[test] xi.imaging/xi.imaging_rw/xi.emit/xi.log @1 — surviving "
                "carved interfaces == legacy fields, byte for byte\n");
    static_assert(XI_ABI_VERSION >= 10, "domain carve assumes the v10 door");

    xi::binary_sink() = &obs_binary;
    xi::status_sink() = &obs_status;

    xi_host_api host = xi::ImagePool::make_host_api();
    CHECK(host.get_interface != nullptr);

    // ---- (1) xi.imaging@1 resolves and matches the legacy fields ------------
    // [ABI v12 — read_image_file was DROPPED from xi.imaging@1 at THE CUT; its
    //  host slot is evicted to the xi.image.decode capability. Only the image
    //  pool entries remain.]
    {
        const auto* iv = static_cast<const xi_imaging_v1*>(
            host.get_interface("xi.imaging", 1));
        CHECK(iv != nullptr);
        if (iv) {
            CHECK(iv->image_create    == host.image_create);
            CHECK(iv->image_addref    == host.image_addref);
            CHECK(iv->image_release   == host.image_release);
            CHECK(iv->image_data      == host.image_data);
            CHECK(iv->image_width     == host.image_width);
            CHECK(iv->image_height    == host.image_height);
            CHECK(iv->image_channels  == host.image_channels);
            CHECK(iv->image_stride    == host.image_stride);
        }
    }

    // ---- (1b) xi.imaging_rw@1 — read/write access discipline (ext review 02 I.4)
    {
        const auto* rw = static_cast<const xi_imaging_rw_v1*>(
            host.get_interface("xi.imaging_rw", 1));
        CHECK(rw != nullptr);
        if (rw) {
            CHECK(rw->image_read  != nullptr);
            CHECK(rw->image_write != nullptr);
            // Bad handle -> null on both.
            CHECK(rw->image_read(XI_IMAGE_NULL)  == nullptr);
            CHECK(rw->image_write(XI_IMAGE_NULL) == nullptr);

            // A freshly-created handle is uniquely owned (refcount == 1): both
            // image_read and image_write hand back the SAME pool bytes as
            // image_data, and the write pointer is non-null.
            xi_image_handle h = host.image_create(4, 4, 1);
            CHECK(h != XI_IMAGE_NULL);
            const uint8_t* rp = rw->image_read(h);
            uint8_t*       wp = rw->image_write(h);
            CHECK(rp != nullptr);
            CHECK(wp != nullptr);
            CHECK(rp == host.image_data(h));   // same bytes as the legacy pointer
            CHECK(wp == host.image_data(h));
            // Writing through the write pointer is visible via the read pointer.
            wp[0] = 0x7E;
            CHECK(rw->image_read(h)[0] == 0x7E);

            // Alias it (a second consumer): refcount now 2 -> image_write must
            // return NULL (a shared input is NOT writable; no copy-on-write),
            // while image_read still works.
            host.image_addref(h);
            CHECK(rw->image_write(h) == nullptr);
            CHECK(rw->image_read(h)  != nullptr);
            CHECK(rw->image_read(h)[0] == 0x7E);
            // Drop the alias -> uniquely owned again -> writable again.
            host.image_release(h);
            CHECK(rw->image_write(h) != nullptr);
            host.image_release(h);
        }
    }

    // ---- (2) [ABI v12 — the xi.doc@1 block was DELETED at THE CUT.] ----------
    // xi_doc_v1 and its doc_chunk_alloc/realloc/free + doc_retain/release/
    // refcount host slots are gone with the Record yyjson-doc dispatch path.
    // get_interface("xi.doc", *) no longer resolves.

    // ---- (3) xi.emit@1 resolves; emit_binary matches ------------------------
    // [ABI v12 — emit_record was DROPPED from xi.emit@1 at THE CUT; a source now
    //  emits a sealed pack via xi_pack_v1::emit_pack. xi.emit@1 carries only the
    //  WS binary push.]
    {
        const auto* ev = static_cast<const xi_emit_v1*>(host.get_interface("xi.emit", 1));
        CHECK(ev != nullptr);
        if (ev) {
            CHECK(ev->emit_binary == host.emit_binary);
        }
    }

    // ---- (3a) xi.emit@2 resolves; emit_binary_owned present (perf/ws-lean) -----
    // The zero-copy owned-emit door is an ADDITIVE supplement to @1; it is NOT a
    // host_api struct field (the v12 layout is frozen), so it has no field twin to
    // match — just assert it resolves and carries the verb.
    {
        const auto* ev2 = static_cast<const xi_emit_v2*>(host.get_interface("xi.emit", 2));
        CHECK(ev2 != nullptr);
        if (ev2) CHECK(ev2->emit_binary_owned != nullptr);
    }

    // ---- (3b) freeze-guard: every carved entry tracks its struct-field twin --
    // [ABI v12 — install_trigger_hook + the emit_record forwarder were DELETED
    //  at THE CUT, so there is no longer a "wired" table to build; the carved
    //  interfaces are pure pointer copies of make_host_api()'s fields.]
    // door_matches_fields survives and asserts every surviving carved interface
    // fn-pointer equals its xi_host_api struct-field twin, so the door and the
    // field can never silently drift onto different code paths.
    {
        CHECK(xi::ImagePool::door_matches_fields(host));
    }

    // ---- (4) xi.log@1 resolves and matches -----------------------------------
    {
        const auto* lv = static_cast<const xi_log_v1*>(host.get_interface("xi.log", 1));
        CHECK(lv != nullptr);
        if (lv) {
            CHECK(lv->log        == host.log);
            CHECK(lv->set_status == host.set_status);
        }
    }

    // ---- (5) xi.legacy@9 is RETIRED (Phase 4) — the door no longer answers it --
    // The whole-table legacy view was retired in Phase 4 (core_fix_plan.md §12):
    // capabilities are reached via the carved interfaces above or the struct
    // fields directly, never the legacy passthrough.
    {
        CHECK(host.get_interface("xi.legacy", 9) == nullptr);
    }

    // ---- (6) unknown id / wrong version -> null ------------------------------
    CHECK(host.get_interface("xi.imaging", 2) == nullptr);
    CHECK(host.get_interface("xi.log", 99)    == nullptr);  // live domain, wrong version
    CHECK(host.get_interface("xi.emit", 99)   == nullptr);
    CHECK(host.get_interface("xi.nope", 1)    == nullptr);

    // ---- (7) SDK wrappers: v10 host uses the interface -----------------------
    {
        xi::Plugin plug(&host, "domains_v10");
        // emit: emit_binary forwards to the installed binary sink.
        g_binary_calls = 0;
        std::vector<uint8_t> frame{1, 2, 3, 4};
        plug.emit_binary(frame);
        CHECK(g_binary_calls == 1);
        CHECK(g_binary_len == 4);
        // log: status forwards to the installed status sink.
        plug.status("running");
        CHECK(g_status_src == "domains_v10");
        CHECK(g_status_txt == "running");
        // imaging_rw: image_read/image_write via the SDK wrapper honour the
        // uniqueness gate (write null for a shared handle).
        xi_image_handle h = host.image_create(2, 2, 1);
        CHECK(h != XI_IMAGE_NULL);
        CHECK(plug.image_write(h) != nullptr);          // uniquely owned -> writable
        CHECK(plug.image_read(h)  == host.image_data(h));
        host.image_addref(h);
        CHECK(plug.image_write(h) == nullptr);          // shared -> not writable
        host.image_release(h);
        host.image_release(h);
    }

    // ---- (8) SDK wrappers: simulated pre-v10 host falls back to fields -------
    {
        xi_host_api old_host = host;
        old_host.get_interface = nullptr;            // pre-v10: no query door
        xi::Plugin plug(&old_host, "domains_legacy");
        // All wrappers must fall back to the legacy fields and behave identically.
        g_binary_calls = 0;
        std::vector<uint8_t> frame{9, 9};
        plug.emit_binary(frame);
        CHECK(g_binary_calls == 1);
        CHECK(g_binary_len == 2);
        plug.status("legacy-path");
        CHECK(g_status_src == "domains_legacy");
        CHECK(g_status_txt == "legacy-path");
        // imaging_rw: no door -> fall back to the legacy image_data pointer for
        // BOTH read and write (the by-convention discipline; no uniqueness gate).
        xi_image_handle h = host.image_create(2, 2, 1);
        CHECK(h != XI_IMAGE_NULL);
        CHECK(plug.image_read(h)  == host.image_data(h));
        CHECK(plug.image_write(h) == host.image_data(h));
        host.image_release(h);
    }

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
