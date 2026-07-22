//
// record_save_pack_test.cpp — polaris2 gate P3 (docs/new_gen/10): record_save
// PERSISTENCE PARITY, end to end against the REAL record_save DLL.
//
//   * DOOR PROBE — record_save (a sink) now publishes xi.pack@1: it consumes packs.
//   * PERSIST == SHARED ENCODER — the pack door writes ONE <base>.xex1 file whose
//     bytes EQUAL xi::xex1::encode_pack_v3 over the same pack (the SAME encoder
//     expose pushes on the wire). Byte-for-byte: record_save did not fork a second
//     dump implementation (memory ≈ wire ≈ disk, doc 07).
//   * REPLAY LOAD-BACK — the file is read back through xex1_pack_load.hpp (the
//     untrusted-disk ingress edge). The rebuilt pack's entries, nested msgpack
//     bytes, and image PIXELS are identical to the original; channel/seq are lifted.
//   * POOL BALANCE — the input pack, the ack, and the loaded pack all release
//     their pooled image handles; ImagePool + PackRegistry return to baseline.
//
// (v12: the legacy Record door + its .json/.bmp writer are deleted — the .xex1
// pack dump is the sink, so there is no Record-door parity leg any more.)
//
// LoadLibraryA/GetProcAddress/HMODULE/GetLastError for the loader below:
// real <windows.h> on Windows, dlopen/dlsym shim on POSIX (same names).
#include <xi/xi_dynlib.hpp>

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (has_pack_door / run_pack_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_pack_contract.hpp>  // reserved keys: xi::pack_contract::kChannel/kSeq
#include <xi/xi_mp.hpp>             // xi::mp::Writer (build the nested-mp fixture entry)

#include "xex1_pack_dump.hpp"       // encode_pack_v3 — the shared encoder (the golden oracle)
#include "xex1_pack_load.hpp"       // load_pack_v3_file — the replay seed under test

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef RECORD_SAVE_DLL_PATH
#define RECORD_SAVE_DLL_PATH "xi-record_save.dll"
#endif

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)
#define SECTION(name) std::printf("[test] %s\n", name)

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

// Compare two host packs entry-for-entry (same key order, tag, and value); the
// `orig` side skips the reserved $channel/$seq keys (lifted to the frame header,
// never dumped as entries), which is exactly the set the dump omits.
static void expect_entries_identical(const xi::Pack& orig, const xi::Pack& loaded) {
    std::vector<size_t> orig_idx;
    for (size_t i = 0; i < orig.size(); ++i) {
        std::string_view k = orig.key_at(i);
        if (k == xi::pack_contract::kChannel || k == xi::pack_contract::kSeq) continue;
        orig_idx.push_back(i);
    }
    CHECK(orig_idx.size() == loaded.size());
    if (orig_idx.size() != loaded.size()) return;

    for (size_t j = 0; j < loaded.size(); ++j) {
        const size_t i = orig_idx[j];
        std::string_view ok = orig.key_at(i), lk = loaded.key_at(j);
        CHECK(ok == lk);
        xi::PackTag ot = orig.tag_at(i), lt = loaded.tag_at(j);
        CHECK(ot == lt);
        if (ok != lk || ot != lt) continue;
        std::string key(lk);
        switch (lt) {
            case xi::PackTag::I64:  CHECK(*orig.get_i64(key) == *loaded.get_i64(key)); break;
            case xi::PackTag::F64:  CHECK(*orig.get_f64(key) == *loaded.get_f64(key)); break;
            case xi::PackTag::Bool: CHECK(*orig.get_bool(key) == *loaded.get_bool(key)); break;
            case xi::PackTag::Str:  CHECK(*orig.get_str(key) == *loaded.get_str(key)); break;
            case xi::PackTag::Bin: {
                auto a = *orig.get_bin(key); auto b = *loaded.get_bin(key);
                CHECK(std::vector<uint8_t>(a.begin(), a.end()) ==
                      std::vector<uint8_t>(b.begin(), b.end()));
                break;
            }
            case xi::PackTag::Mp: {
                auto a = *orig.get_mp(key); auto b = *loaded.get_mp(key);
                CHECK(std::vector<uint8_t>(a.begin(), a.end()) ==
                      std::vector<uint8_t>(b.begin(), b.end()));
                break;
            }
            case xi::PackTag::Blob: {
                // Self-describing blob (spec 30): the descriptor + payload must
                // round-trip byte-identical (memory == wire == disk).
                auto a = *orig.get_blob(key); auto b = *loaded.get_blob(key);
                CHECK(std::vector<uint8_t>(a.desc.begin(), a.desc.end()) ==
                      std::vector<uint8_t>(b.desc.begin(), b.desc.end()));
                CHECK(std::vector<uint8_t>(a.payload.begin(), a.payload.end()) ==
                      std::vector<uint8_t>(b.payload.begin(), b.payload.end()));
                break;
            }
            default: break;
        }
    }
}

int main() {
    std::printf("[test] record_save pack door — persistence parity (P3)\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    const xi_pack_v1* fi = xi::pack_v1_iface();

    const int base_live = xi::ImagePool::instance().cumulative().live_now;

    // Isolated output dir under the OS temp root.
    std::filesystem::path outdir =
        std::filesystem::temp_directory_path() / "xinsp2_record_save_pack_test";
    std::error_code ec;
    std::filesystem::remove_all(outdir, ec);

    HMODULE dll = LoadLibraryA(RECORD_SAVE_DLL_PATH);
    if (!dll) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", RECORD_SAVE_DLL_PATH, GetLastError()); return 1; }
    auto factory  = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(dll, "xi_plugin_create"));
    auto get_iface = reinterpret_cast<xi_plugin_get_interface_fn>(GetProcAddress(dll, "xi_plugin_get_interface"));
    CHECK(factory != nullptr);
    CHECK(get_iface != nullptr);
    if (!factory) return 1;
    void* inst = factory(&host, "rec0");
    CHECK(inst != nullptr);
    if (!inst) return 1;
    auto rec = std::make_unique<xi::CAbiInstanceAdapter>("rec0", "rec0", dll, inst);

    // ---------------------------------------------------------------------
    // (A) Door probe.
    // ---------------------------------------------------------------------
    SECTION("door probe: record_save answers xi.pack@1 (it consumes packs)");
    CHECK(get_iface && get_iface("xi.pack", 1) != nullptr);
    CHECK(rec->has_pack_door());

    // Enable saving into the isolated dir with a deterministic name.
    std::string def = std::string("{\"enabled\":true,\"naming_rule\":\"cap_{count}\",\"output_dir\":\"")
                      + outdir.generic_string() + "\"}";
    CHECK(rec->set_def(def));

    // Build one multi-entry input pack: reserved $channel/$seq + scalars + str +
    // bin + nested msgpack + a pooled image (the doc 07 D1/D2/D3 mix).
    std::vector<uint8_t> pixels = {10, 20, 30, 40};   // 2x2x1 gray
    std::vector<uint8_t> raw    = {0xDE, 0xAD, 0xBE, 0xEF};
    xi::mp::Writer blobs;
    blobs.array(2);
    blobs.map(2); blobs.key("area"); blobs.int_(100); blobs.key("cx"); blobs.float_(2.5);
    blobs.map(2); blobs.key("area"); blobs.int_(250); blobs.key("cx"); blobs.float_(8.0);
    std::vector<uint8_t> blobs_mp = blobs.bytes();

    auto build_input = [&]() -> xi_pack_handle {
        xi_pack_builder b = fi->builder_new();
        fi->builder_add_str(b, "$channel", "cam0", 4);
        fi->builder_add_i64(b, "$seq", 7);
        fi->builder_add_i64(b, "count", 42);
        fi->builder_add_f64(b, "score", 1.5);
        fi->builder_add_bool(b, "pass", 1);   // bool entry: dump + load-back parity
        fi->builder_add_str(b, "label", "ok", 2);
        fi->builder_add_bin(b, "raw", raw.data(), (int32_t)raw.size());
        fi->builder_add_mp(b, "blobs", blobs_mp.data(), (int32_t)blobs_mp.size());
        fi->builder_add_image(b, "frame", 2, 2, 1, pixels.data());
        return fi->builder_seal(b);
    };

    // ---------------------------------------------------------------------
    // (B) Persist == the shared encoder's output, byte-for-byte.
    // ---------------------------------------------------------------------
    SECTION("persist: the .xex1 file bytes equal encode_pack_v3 (shared encoder)");
    std::string saved_path;
    {
        xi_pack_handle in = build_input();

        // The golden oracle: the SAME shared walk+encoder, in-process over the same
        // pack. Pass the @4 blob door so PackIn::blob() resolves (a blob dump needs it).
        xi::PackIn view(fi, in, xi::pack_v4_iface());
        std::vector<uint8_t> expected = xi::xex1::encode_pack_v3(view, "cam0", 7);

        xi_pack_handle ack = rec->run_pack_door(in);
        CHECK(ack != XI_PACK_NULL);
        std::string base_name;
        if (ack) {
            // "saved" is a REAL bool entry now (PackOut::boolean emits the bool tag).
            int32_t saved = 0; CHECK(fi->get_bool(ack, "saved", &saved) == 1 && saved == 1);
            const char* bn = nullptr; int32_t bl = 0;
            CHECK(fi->get_str(ack, "base_name", &bn, &bl) == 1);
            if (bn) base_name.assign(bn, bl);
            int64_t bytes = 0;
            CHECK(fi->get_i64(ack, "bytes", &bytes) == 1 && bytes == (int64_t)expected.size());
            fi->release(ack);
        }
        CHECK(base_name == "cap_000001.xex1");
        saved_path = (outdir / base_name).generic_string();

        std::vector<uint8_t> on_disk = read_file(saved_path);
        CHECK(!on_disk.empty());
        CHECK(on_disk == expected);   // record_save wrote the shared encoder's bytes

        fi->release(in);
    }

    // ---------------------------------------------------------------------
    // (C) Replay: load the file back through the untrusted-disk ingress.
    // ---------------------------------------------------------------------
    SECTION("replay: load-back rebuilds an identical pack (entries + image pixels)");
    {
        xi_pack_handle in = build_input();
        const xi::Pack* orig = xi::PackRegistry::instance().pack(in);
        CHECK(orig != nullptr);

        xi::xex1::LoadResult loaded = xi::xex1::load_pack_v3_file(saved_path);
        CHECK(loaded.ok);
        if (!loaded.ok) std::fprintf(stderr, "  load error: %s\n", loaded.error.c_str());
        CHECK(loaded.channel == "cam0");   // lifted from $channel
        CHECK(loaded.seq == 7);            // lifted from $seq
        if (orig && loaded.ok) expect_entries_identical(*orig, loaded.pack);

        fi->release(in);
        // loaded (and its pooled image handle) drops at scope end.
    }

    // ---------------------------------------------------------------------
    // (D) Corrupt / truncated disk is refused, not trusted.
    // ---------------------------------------------------------------------
    SECTION("untrusted disk: a truncated dump is rejected by ingress");
    {
        std::vector<uint8_t> good = read_file(saved_path);
        std::vector<uint8_t> bad(good.begin(), good.begin() + good.size() / 2);
        xi::xex1::LoadResult r = xi::xex1::load_pack_v3(bad.data(), bad.size());
        CHECK(!r.ok);   // bounded ingress catches the truncation
    }

    // ---- teardown + balance ----
    rec.reset();             // ~CAbiInstanceAdapter -> xi_plugin_destroy + leak sweep
    FreeLibrary(dll);
    std::filesystem::remove_all(outdir, ec);

    CHECK(xi::ImagePool::instance().cumulative().live_now == base_live);
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
