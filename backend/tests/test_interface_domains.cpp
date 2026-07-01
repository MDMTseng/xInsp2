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

    // ---- (3) xi.emit@1 resolves and matches ----------------------------------
    {
        const auto* ev = static_cast<const xi_emit_v1*>(host.get_interface("xi.emit", 1));
        CHECK(ev != nullptr);
        if (ev) {
            // emit_record is wired at runtime by install_trigger_hook into the
            // LIVE table, not the canonical one — so on a bare make_host_api()
            // table both are null and still match byte-for-byte (consistent with
            // xi.legacy@9's emit_record).
            CHECK(ev->emit_record == host.emit_record);
            CHECK(ev->emit_binary == host.emit_binary);
        }
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
    }

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
