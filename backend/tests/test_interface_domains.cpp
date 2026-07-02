//
// test_interface_domains.cpp — Phase 3 proof: the remaining capability domains
// (xi.imaging@1 / xi.doc@1 / xi.emit@1 / xi.log@1) are carved out of the v9
// monolith as frozen, segregated interfaces reached through host->get_interface,
// each one byte-for-byte IDENTICAL to the legacy xi_host_api fields it groups.
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
#include <xi/xi_trigger_bus.hpp>  // xi::install_trigger_hook (wires emit_record)
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

// A sentinel read_image_file so the imaging interface has a non-null entry to
// compare (and the SDK read_image_file wrapper has something to forward to).
static xi_image_handle fake_read_image(const char* path) {
    return path && std::strcmp(path, "ok") == 0 ? (xi_image_handle)0xABCD : XI_IMAGE_NULL;
}

// Observers for the emit/log domains so the SDK wrappers can be seen to forward.
static int     g_binary_calls = 0;
static int     g_binary_len   = 0;
static void    obs_binary(const void* /*d*/, int len) { ++g_binary_calls; g_binary_len = len; }
static std::string g_status_src, g_status_txt;
static void    obs_status(const char* s, const char* t) {
    g_status_src = s ? s : ""; g_status_txt = t ? t : "";
}

int main() {
    std::printf("[test] xi.imaging/xi.doc/xi.emit/xi.log @1 — carved interfaces "
                "== legacy fields, byte for byte\n");
    static_assert(XI_ABI_VERSION >= 10, "domain carve assumes the v10 door");

    // Install a reader BEFORE building the table so read_image_file is non-null in
    // both the test's host table and the host's canonical (interface-backing)
    // table — they must capture the same installed pointer to match.
    xi::ImagePool::install_read_image_file(&fake_read_image);
    xi::binary_sink() = &obs_binary;
    xi::status_sink() = &obs_status;

    xi_host_api host = xi::ImagePool::make_host_api();
    CHECK(host.get_interface != nullptr);

    // ---- (1) xi.imaging@1 resolves and matches the legacy fields ------------
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
            CHECK(iv->read_image_file == host.read_image_file);
            CHECK(iv->read_image_file == &fake_read_image);
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

    // ---- (2) xi.doc@1 resolves and matches -----------------------------------
    {
        const auto* dv = static_cast<const xi_doc_v1*>(host.get_interface("xi.doc", 1));
        CHECK(dv != nullptr);
        if (dv) {
            CHECK(dv->doc_chunk_alloc   == host.doc_chunk_alloc);
            CHECK(dv->doc_chunk_realloc == host.doc_chunk_realloc);
            CHECK(dv->doc_chunk_free    == host.doc_chunk_free);
            CHECK(dv->doc_retain        == host.doc_retain);
            CHECK(dv->doc_release       == host.doc_release);
            CHECK(dv->doc_refcount      == host.doc_refcount);
        }
    }

    // ---- (3) xi.emit@1 resolves; emit_binary matches, emit_record is a LIVE door
    {
        const auto* ev = static_cast<const xi_emit_v1*>(host.get_interface("xi.emit", 1));
        CHECK(ev != nullptr);
        if (ev) {
            CHECK(ev->emit_binary == host.emit_binary);
            // emit_record is served as a STABLE forwarder (not the raw field): the
            // door hands out a NON-NULL pointer even on a bare table, so a future
            // plugin that trusts the door never gets the old permanent null. The
            // forwarder reads a slot install_trigger_hook publishes (see 3b).
            CHECK(ev->emit_record != nullptr);
        }
    }

    // ---- (3b) on a FULLY WIRED table the door's emit_record reaches the bus ----
    // install_trigger_hook wires host.emit_record AND publishes it into the slot
    // the door's forwarder reads. The freeze-guard proves every carved entry still
    // tracks its struct-field twin (emit_record functionally: slot == wired field).
    {
        xi_host_api wired = xi::ImagePool::make_host_api();
        xi::install_trigger_hook(wired);
        CHECK(wired.emit_record != nullptr);
        const auto* ev = static_cast<const xi_emit_v1*>(wired.get_interface("xi.emit", 1));
        CHECK(ev != nullptr);
        if (ev) {
            CHECK(ev->emit_record != nullptr);            // the stable forwarder
            CHECK(ev->emit_binary == wired.emit_binary);  // still the same field
        }
        // The forwarder's target (published slot) now equals the wired field, and
        // every other carved entry equals its struct field — freeze-guard green.
        CHECK(xi::ImagePool::door_matches_fields(wired));
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
    CHECK(host.get_interface("xi.doc", 0)     == nullptr);
    CHECK(host.get_interface("xi.emit", 99)   == nullptr);
    CHECK(host.get_interface("xi.nope", 1)    == nullptr);

    // ---- (7) SDK wrappers: v10 host uses the interface -----------------------
    {
        xi::Plugin plug(&host, "domains_v10");
        // imaging: read_image_file forwards to the installed reader.
        CHECK(plug.read_image_file("ok") == (xi_image_handle)0xABCD);
        CHECK(plug.read_image_file("no") == XI_IMAGE_NULL);
        // doc: alloc/refcount roundtrip through the host allocator.
        void* p = plug.doc_chunk_alloc(64);
        CHECK(p != nullptr);
        p = plug.doc_chunk_realloc(p, 128);
        CHECK(p != nullptr);
        plug.doc_chunk_free(p);
        CHECK(plug.doc_refcount(nullptr) == 0);   // unregistered doc -> 0
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
        CHECK(plug.read_image_file("ok") == (xi_image_handle)0xABCD);
        void* p = plug.doc_chunk_alloc(32);
        CHECK(p != nullptr);
        plug.doc_chunk_free(p);
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
