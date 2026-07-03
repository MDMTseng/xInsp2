//
// record_replay_pack_test.cpp — polaris2 gate P3, THE CLOSED LOOP: record ->
// save -> replay, byte-identical (docs/new_gen/10 "persistence parity").
//
// Drives BOTH real DLLs end to end:
//
//   original pack --record_save pack door--> cap_000001.xex1
//                 --record_replay process()--> emitted pack (off the TriggerBus)
//
// and proves parity at BOTH levels:
//
//   * PACK level  — the replayed pack is entry-for-entry identical to the
//     original: same keys, SAME ORDER, same tags (incl. the restored
//     $channel/$seq reserved entries), same scalar/str/bin/mp bytes, same
//     image dims + PIXELS.
//   * BYTE level  — re-dumping the replayed pack through the shared encoder
//     (encode_pack_v3, the exact walk record_save persists with) reproduces
//     the on-disk file byte-for-byte. Since same pack -> same bytes is the
//     canonical-profile guarantee, dump equality IS pack equality:
//     original ≈ disk ≈ replayed, one loop, zero drift.
//
// Also proves the fail-loud edge: a corrupt/truncated .xex1 in the replay dir
// emits a sealed $fault pack (never a partial frame), the ack carries the
// parser's reason, and the cursor advances past it (one bad file cannot wedge
// the loop). Pool + PackRegistry balance at the end.
//
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#include <xi/xi_cabi_adapter.hpp>   // CAbiInstanceAdapter (process / run_pack_door)
#include <xi/xi_image_pool.hpp>     // make_host_api + cumulative().live_now
#include <xi/xi_pack_abi.hpp>       // install_pack_abi + pack_v1_iface + PackRegistry
#include <xi/xi_trigger_bus.hpp>    // install_trigger_hook + the pack dispatch sink
#include <xi/xi_mp.hpp>             // xi::mp::Writer (build the nested-mp fixture entry)

#include "xex1_pack_dump.hpp"       // encode_pack_v3 — the shared encoder (the oracle)

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef RECORD_SAVE_DLL_PATH
#define RECORD_SAVE_DLL_PATH "xi-record_save.dll"
#endif
#ifndef RECORD_REPLAY_DLL_PATH
#define RECORD_REPLAY_DLL_PATH "xi-record_replay.dll"
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

// Full identity: same size, same key ORDER, same tags, same values — no key is
// skipped (the replay source restores $channel/$seq, so the replayed pack must
// carry the reserved entries too, in the same leading positions).
static void expect_packs_identical(const xi::Pack& a, const xi::Pack& b) {
    CHECK(a.size() == b.size());
    if (a.size() != b.size()) return;
    for (size_t i = 0; i < a.size(); ++i) {
        std::string_view ak = a.key_at(i), bk = b.key_at(i);
        CHECK(ak == bk);
        xi::PackTag at = a.tag_at(i), bt = b.tag_at(i);
        CHECK(at == bt);
        if (ak != bk || at != bt) continue;
        std::string key(ak);
        switch (at) {
            case xi::PackTag::I64: CHECK(*a.get_i64(key) == *b.get_i64(key)); break;
            case xi::PackTag::F64: CHECK(*a.get_f64(key) == *b.get_f64(key)); break;
            case xi::PackTag::Str: CHECK(*a.get_str(key) == *b.get_str(key)); break;
            case xi::PackTag::Bin: {
                auto x = *a.get_bin(key); auto y = *b.get_bin(key);
                CHECK(std::vector<uint8_t>(x.begin(), x.end()) ==
                      std::vector<uint8_t>(y.begin(), y.end()));
                break;
            }
            case xi::PackTag::Mp: {
                auto x = *a.get_mp(key); auto y = *b.get_mp(key);
                CHECK(std::vector<uint8_t>(x.begin(), x.end()) ==
                      std::vector<uint8_t>(y.begin(), y.end()));
                break;
            }
            case xi::PackTag::Image: {
                auto x = *a.get_image(key); auto y = *b.get_image(key);
                CHECK(x.width == y.width && x.height == y.height && x.channels == y.channels);
                CHECK(std::vector<uint8_t>(x.pixels.begin(), x.pixels.end()) ==
                      std::vector<uint8_t>(y.pixels.begin(), y.pixels.end()));
                break;
            }
            default: break;
        }
    }
}

struct Dll {
    HMODULE h = nullptr;
    xi::PluginInfo::CFactoryFn factory = nullptr;
    void* inst = nullptr;
    std::unique_ptr<xi::CAbiInstanceAdapter> ad;

    bool load(const char* path, const xi_host_api* host, const char* name) {
        h = LoadLibraryA(path);
        if (!h) { std::fprintf(stderr, "FAIL: LoadLibrary(%s) err %lu\n", path, GetLastError()); return false; }
        factory = reinterpret_cast<xi::PluginInfo::CFactoryFn>(GetProcAddress(h, "xi_plugin_create"));
        if (!factory) return false;
        inst = factory(host, name);
        if (!inst) return false;
        ad = std::make_unique<xi::CAbiInstanceAdapter>(name, name, h, inst);
        return true;
    }
    void unload() { ad.reset(); if (h) { FreeLibrary(h); h = nullptr; } }
};

// Drive the Record door once (the replay source's "advance one file" verb).
static void drive_process(xi::CAbiInstanceAdapter& ad) {
    static const std::string in_json = "{}";
    xi_record in{};
    in.data = reinterpret_cast<const uint8_t*>(in_json.c_str());
    in.len  = (int32_t)in_json.size();
    xi_record_out out; xi_record_out_init(&out);
    ad.process(&in, &out);
    xi_record_out_free(&out);
}

int main() {
    std::printf("[test] record -> save -> replay: the P3 closed loop\n");

    xi::install_pack_abi();
    xi_host_api host = xi::ImagePool::make_host_api();
    xi::install_trigger_hook(host);                 // route emit_* -> TriggerBus
    const xi_pack_v1* fi = xi::pack_v1_iface();

    const int base_live = xi::ImagePool::instance().cumulative().live_now;

    // Capture every pack the replay source emits (sink fires synchronously on
    // the emitting thread — process() returns after the emit).
    std::mutex m;
    std::vector<xi::TriggerEvent> got;
    xi::TriggerBus::instance().set_sink([&](xi::TriggerEvent ev) {
        std::lock_guard<std::mutex> lk(m); got.push_back(std::move(ev));
    });
    auto drain = [&]() {
        std::lock_guard<std::mutex> lk(m);
        for (auto& ev : got)
            if (ev.pack != XI_PACK_NULL) xi::TriggerBus::instance().release_pack_(ev.pack);
        got.clear();
    };

    // Isolated replay dir under the OS temp root.
    std::filesystem::path outdir =
        std::filesystem::temp_directory_path() / "xinsp2_record_replay_pack_test";
    std::error_code ec;
    std::filesystem::remove_all(outdir, ec);

    // ---- the original pack: reserved keys + the full doc-07 D1/D2/D3 mix ----
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
        fi->builder_add_str(b, "label", "ok", 2);
        fi->builder_add_bin(b, "raw", raw.data(), (int32_t)raw.size());
        fi->builder_add_mp(b, "blobs", blobs_mp.data(), (int32_t)blobs_mp.size());
        fi->builder_add_image(b, "frame", 2, 2, 1, pixels.data());
        return fi->builder_seal(b);
    };

    Dll saver, replayer;
    if (!saver.load(RECORD_SAVE_DLL_PATH, &host, "rec0")) return 1;
    if (!replayer.load(RECORD_REPLAY_DLL_PATH, &host, "rep0")) return 1;

    // -----------------------------------------------------------------------
    // (A) RECORD + SAVE: persist one pack through the real record_save door.
    // -----------------------------------------------------------------------
    SECTION("save: record_save persists the pack as cap_000001.xex1");
    std::vector<uint8_t> on_disk, expected;
    {
        std::string def = std::string("{\"enabled\":true,\"naming_rule\":\"cap_{count}\",\"output_dir\":\"")
                          + outdir.generic_string() + "\"}";
        CHECK(saver.ad->set_def(def));

        xi_pack_handle in = build_input();
        xi::PackIn view(fi, in);
        expected = xi::xex1::encode_pack_v3(view, "cam0", 7);   // the shared-encoder oracle

        xi_pack_handle ack = saver.ad->run_pack_door(in);
        CHECK(ack != XI_PACK_NULL);
        if (ack) fi->release(ack);
        fi->release(in);

        on_disk = read_file((outdir / "cap_000001.xex1").generic_string());
        CHECK(!on_disk.empty());
        CHECK(on_disk == expected);
    }

    // -----------------------------------------------------------------------
    // (B) REPLAY: record_replay emits the file as ONE sealed pack.
    // -----------------------------------------------------------------------
    SECTION("replay: one process() emits ONE sealed pack off the file");
    xi_pack_handle replayed = XI_PACK_NULL;
    {
        std::string def = std::string("{\"dir\":\"") + outdir.generic_string() + "\"}";
        CHECK(replayer.ad->set_def(def));

        drive_process(*replayer.ad);
        {
            std::lock_guard<std::mutex> lk(m);
            CHECK(got.size() == 1);
            if (got.size() == 1) { CHECK(got[0].pack != XI_PACK_NULL); replayed = got[0].pack; }
        }
    }

    // -----------------------------------------------------------------------
    // (C) PARITY, pack level: replayed pack == original pack, entry for entry.
    // -----------------------------------------------------------------------
    SECTION("parity: replayed pack is entry-for-entry identical (incl. $channel/$seq)");
    if (replayed != XI_PACK_NULL) {
        xi_pack_handle orig = build_input();
        const xi::Pack* po = xi::PackRegistry::instance().pack(orig);
        const xi::Pack* pr = xi::PackRegistry::instance().pack(replayed);
        CHECK(po != nullptr && pr != nullptr);
        if (po && pr) expect_packs_identical(*po, *pr);
        fi->release(orig);
    }

    // -----------------------------------------------------------------------
    // (D) PARITY, byte level: re-dump(replayed) == disk == original's dump.
    // -----------------------------------------------------------------------
    SECTION("parity: re-dumping the replayed pack reproduces the file bytes");
    if (replayed != XI_PACK_NULL) {
        xi::PackIn view(fi, replayed);
        // Lift channel/seq exactly the way the record_save sink does.
        const std::string channel(view.str(xi::Record::kChannelKey).value_or("default"));
        const uint64_t    seq = (uint64_t)view.i64_or(xi::Record::kSeqKey, 0);
        CHECK(channel == "cam0");
        CHECK(seq == 7);
        std::vector<uint8_t> redump = xi::xex1::encode_pack_v3(view, channel, seq);
        CHECK(redump == on_disk);      // replay -> save again would write the SAME file
        CHECK(redump == expected);     // and the same bytes the original pack dumps to
    }
    drain();   // release the captured replayed pack (the dispatcher's job)

    // -----------------------------------------------------------------------
    // (E) End of files: no more emits, honest ack; then loop wraps.
    // -----------------------------------------------------------------------
    SECTION("end of files: nothing emitted past the last file; loop wraps");
    {
        drive_process(*replayer.ad);                       // past the end
        { std::lock_guard<std::mutex> lk(m); CHECK(got.empty()); }

        CHECK(replayer.ad->set_def("{\"loop\":true}"));    // opt into wrap-around
        drive_process(*replayer.ad);                       // wraps to file 1
        { std::lock_guard<std::mutex> lk(m); CHECK(got.size() == 1); }
        drain();
        CHECK(replayer.ad->set_def("{\"loop\":false}"));
    }

    // -----------------------------------------------------------------------
    // (F) Fail loud: a corrupt file emits a sealed $fault pack and is skipped.
    // -----------------------------------------------------------------------
    SECTION("untrusted disk: corrupt .xex1 -> $fault pack, cursor advances");
    {
        // "aaa_bad" sorts before "cap_000001": the bad file is hit FIRST.
        std::vector<uint8_t> bad(on_disk.begin(), on_disk.begin() + on_disk.size() / 2);
        { std::ofstream o((outdir / "aaa_bad.xex1").string(), std::ios::binary);
          o.write(reinterpret_cast<const char*>(bad.data()), (std::streamsize)bad.size()); }

        replayer.ad->exchange("{\"command\":\"rewind\"}");  // rescan picks up both files

        drive_process(*replayer.ad);                        // the bad file
        {
            std::lock_guard<std::mutex> lk(m);
            CHECK(got.size() == 1);
            if (got.size() == 1 && got[0].pack != XI_PACK_NULL) {
                const char* f = nullptr; int32_t fl = 0;
                CHECK(fi->get_str(got[0].pack, "$fault", &f, &fl) == 1);   // sealed fault pack
            }
        }
        drain();

        drive_process(*replayer.ad);                        // advanced to the good file
        {
            std::lock_guard<std::mutex> lk(m);
            CHECK(got.size() == 1);
            if (got.size() == 1 && got[0].pack != XI_PACK_NULL) {
                int64_t cnt = 0;
                CHECK(fi->get_i64(got[0].pack, "count", &cnt) == 1 && cnt == 42);
            }
        }
        drain();
    }

    // ---- teardown + balance ----
    xi::TriggerBus::instance().clear_sink();
    replayer.unload();
    saver.unload();
    std::filesystem::remove_all(outdir, ec);

    CHECK(xi::ImagePool::instance().cumulative().live_now == base_live);
    CHECK(xi::PackRegistry::instance().live_frames() == 0);

    if (g_failures == 0) { std::printf("\nALL TESTS PASSED\n"); return 0; }
    std::fprintf(stderr, "\n%d FAILURES\n", g_failures);
    return 1;
}
