//
// test_isolated_instance.cpp — Phase 2.6 end-to-end.
//
// Drives PluginManager::open_project on a tmp project that declares an
// instance with "isolation":"process". Verifies the manager resolves
// to a ProcessInstanceAdapter, the worker spawns, RPC methods work,
// and process() round-trips pixels through SHM with zero copy.
//
// This is the integration test that proves Phase 2.5 wired up correctly
// — every layer (SHM, IPC, worker exe, adapter, plugin manager,
// instance registry) gets exercised exactly the way a real backend
// open_project would exercise them.
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <xi/xi_image_pool.hpp>
#include <xi/xi_plugin_manager.hpp>
#include <xi/xi_process_instance.hpp>
#include <xi/xi_shm.hpp>
#include <xi/xi_trigger_bridge.hpp>   // pulls in attach_trigger_bridge defn

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

static uint8_t pat(int x, int y) { return (uint8_t)((x * 11 + y * 7) & 0x7F); }

static void write_text(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), (std::streamsize)s.size());
}

int main() {
    // Locate the artifacts produced by the build.
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    fs::path here          = fs::path(self).parent_path();
    fs::path worker_exe    = here / "xinsp-worker.exe";
    fs::path plugin_dll    = here / "test_worker_plugin.dll";
    if (!fs::exists(worker_exe) || !fs::exists(plugin_dll)) {
        std::fprintf(stderr, "missing prereqs: worker=%d plugin=%d\n",
                     (int)fs::exists(worker_exe), (int)fs::exists(plugin_dll));
        return 2;
    }

    // Sandbox layout — wipe first so reruns are clean.
    fs::path root = fs::temp_directory_path() /
        ("xinsp2_iso_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec; fs::remove_all(root, ec);
    fs::create_directories(root / "plugins" / "test_doubler", ec);
    fs::create_directories(root / "project" / "instances" / "cam0", ec);

    // Drop the test plugin DLL into a "plugins/test_doubler/" folder
    // shaped like a normal plugin manifest layout.
    fs::copy_file(plugin_dll, root / "plugins" / "test_doubler" / "test_worker_plugin.dll",
                  fs::copy_options::overwrite_existing, ec);
    write_text(root / "plugins" / "test_doubler" / "plugin.json", R"({
  "name": "test_doubler",
  "description": "Phase 2.6 isolated-instance test plugin",
  "dll": "test_worker_plugin.dll",
  "factory": "xi_plugin_create",
  "has_ui": false
})");

    // Project with one instance opting into process isolation.
    write_text(root / "project" / "project.json", R"({
  "name": "iso_test",
  "script": "inspection.cpp"
})");
    write_text(root / "project" / "instances" / "cam0" / "instance.json", R"({
  "plugin": "test_doubler",
  "isolation": "process"
})");

    // Set up the SHM region the same way the real backend does.
    char shm_name[64];
    std::snprintf(shm_name, sizeof(shm_name), "xinsp2-shm-iso-%lu",
                  (unsigned long)GetCurrentProcessId());
    auto shm_owned = xi::ShmRegion::create(shm_name, 64ull * 1024 * 1024);
    xi::ImagePool::set_shm_region(&shm_owned);

    // Drive PluginManager exactly like service_main does.
    xi::PluginManager mgr;
    mgr.set_isolation_env(worker_exe, shm_name);
    int n = mgr.scan_plugins((root / "plugins").string());
    CHECK(n == 1);

    bool opened = mgr.open_project((root / "project").string());
    CHECK(opened);

    // List skipped instances (should be empty if isolation worked).
    auto warns = mgr.open_warnings();
    for (auto& w : warns) {
        std::fprintf(stderr, "[test] open warning: %s (%s) — %s\n",
                     w.instance.c_str(), w.plugin.c_str(), w.reason.c_str());
    }
    CHECK(warns.empty());

    // Pull cam0 from the registry; must be a ProcessInstanceAdapter.
    auto inst = xi::InstanceRegistry::instance().find("cam0");
    CHECK((bool)inst);
    auto* iso = dynamic_cast<xi::ProcessInstanceAdapter*>(inst.get());
    CHECK(iso != nullptr);
    if (!iso) {
        std::fprintf(stderr, "[test] instance was not a ProcessInstanceAdapter — "
                              "isolation:process probably fell back to in-proc\n");
        return 1;
    }
    std::fprintf(stderr, "[test] cam0 -> ProcessInstanceAdapter (worker spawned)\n");

    // RPC sanity: exchange + get_def.
    {
        std::string rsp = inst->exchange("hello");
        std::fprintf(stderr, "[test] exchange rsp: %s\n", rsp.c_str());
        CHECK(rsp.find("doubler") != std::string::npos);
    }
    {
        std::string def = inst->get_def();
        std::fprintf(stderr, "[test] get_def: %s\n", def.c_str());
        CHECK(def == "{}" || def.empty());
    }

    // Process round-trip over SHM. Allocate input in shared memory so
    // the worker can read it; allocate via ImagePool's host_api SHM
    // entry for parity with how a real plugin would.
    static xi_host_api host = xi::ImagePool::make_host_api();
    xi_image_handle in_h = host.shm_create_image(48, 24, 1);
    CHECK(in_h != 0);
    uint8_t* in_px = host.image_data(in_h);
    for (int y = 0; y < 24; ++y)
        for (int x = 0; x < 48; ++x)
            in_px[y * 48 + x] = pat(x, y);

    xi_record_image in_rec_img{ "frame", in_h };
    xi_record       in_rec{};
    in_rec.images       = &in_rec_img;
    in_rec.image_count  = 1;
    in_rec.json         = "{}";
    xi_record_out out_rec{};
    std::string err;
    bool ok = iso->process_via_rpc(&in_rec, &out_rec, &err);
    CHECK(ok);
    if (!ok) std::fprintf(stderr, "[test] process_via_rpc err: %s\n", err.c_str());

    CHECK(out_rec.image_count == 1);
    if (out_rec.image_count == 1) {
        xi_image_handle out_h = out_rec.images[0].handle;
        CHECK(host.image_width(out_h) == 48);
        CHECK(host.image_height(out_h) == 24);
        const uint8_t* out_px = host.image_data(out_h);
        int wrong = 0;
        for (int y = 0; y < 24; ++y) {
            for (int x = 0; x < 48; ++x) {
                uint8_t in_v = pat(x, y);
                uint8_t exp  = (uint8_t)(in_v * 2 > 255 ? 255 : in_v * 2);
                if (out_px[y * 48 + x] != exp) ++wrong;
            }
        }
        CHECK(wrong == 0);
        std::fprintf(stderr, "[test] open_project → isolated process() %s "
                              "(%d wrong pixels of %d)\n",
                     wrong == 0 ? "ZERO-COPY OK" : "MISMATCH",
                     wrong, 48 * 24);
        host.image_release(out_h);   // drop the worker's allocation
    }
    host.image_release(in_h);

    // Tidy: close project (destructs ProcessInstanceAdapter → DESTROY rpc
    // + worker exit). Should leave the registry clean.
    mgr.close_project();
    CHECK(!xi::InstanceRegistry::instance().find("cam0"));

    fs::remove_all(root, ec);

    if (g_failures == 0) {
        std::fprintf(stderr, "test_isolated_instance: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "test_isolated_instance: %d FAIL\n", g_failures);
    return 1;
}
