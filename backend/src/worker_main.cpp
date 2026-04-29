//
// worker_main.cpp — `xinsp-worker.exe` entry point.
//
// Hosts ONE plugin instance in an isolated process. The backend spawns
// us with --pipe and --shm names + the plugin DLL path; we attach the
// SHM region, LoadLibrary the plugin, and serve RPCs from the pipe
// until the backend disconnects (or sends DESTROY).
//
// Crashes here don't take down the backend — that's the whole point of
// running plugins in their own process. The backend will see an EOF on
// the pipe and can spawn a replacement.
//
// CLI:
//
//   xinsp-worker.exe --pipe=<name> --shm=<name> \
//                    --plugin-dll=<path> [--instance=<name>]
//

#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <xi/xi_abi.h>
#include <xi/xi_image_pool.hpp>
#include <xi/xi_ipc.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_shm.hpp>

namespace ipc = xi::ipc;

// Globals shared between the worker's main RPC loop and the source
// plugin's worker thread (when the plugin runs in this isolated
// worker and calls host->emit_trigger). The lambda installed on
// `host.emit_trigger` writes async frames into the pipe on behalf of
// the plugin thread; the main loop writes replies. Win32 named pipes
// allow concurrent read+write on the same handle, but two writers
// must serialise.
static std::mutex g_pipe_write_mu;
static ipc::Pipe* g_pipe_for_async = nullptr;
static xi_host_api g_host_for_async;

// Mutex-protected wrappers around the pipe write helpers. Required
// because the source-plugin's worker thread (when present) writes
// async frames via emit_trigger while the main RPC loop is also
// writing replies. Win32 named pipes tolerate concurrent reads and
// writes against the same handle, but not concurrent writers.
static void mu_send_reply(ipc::Pipe& p, uint32_t seq, uint32_t type,
                          const void* data, uint32_t len) {
    std::lock_guard<std::mutex> lk(g_pipe_write_mu);
    ipc::send_reply(p, seq, type, data, len);
}
static void mu_send_error_reply(ipc::Pipe& p, uint32_t seq,
                                const std::string& msg) {
    std::lock_guard<std::mutex> lk(g_pipe_write_mu);
    ipc::send_error_reply(p, seq, msg);
}

static std::string arg_get(int argc, char** argv, const std::string& key,
                           const std::string& def = "") {
    std::string prefix = "--" + key + "=";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return def;
}

// Minimal C ABI plugin proxy — resolves the entry points we need from
// the loaded DLL once, then delegates calls.
struct PluginProxy {
    HMODULE dll = nullptr;
    void*   inst = nullptr;
    using create_fn   = void* (*)(const xi_host_api*, const char*);
    using destroy_fn  = void  (*)(void*);
    using process_fn  = void  (*)(void*, const xi_record*, xi_record_out*);
    using exchange_fn = int   (*)(void*, const char*, char*, int);
    using get_def_fn  = int   (*)(void*, char*, int);
    using set_def_fn  = int   (*)(void*, const char*);

    create_fn   create   = nullptr;
    destroy_fn  destroy  = nullptr;
    process_fn  process  = nullptr;
    exchange_fn exchange = nullptr;
    get_def_fn  get_def  = nullptr;
    set_def_fn  set_def  = nullptr;

    bool load(const std::string& dll_path) {
        dll = LoadLibraryA(dll_path.c_str());
        if (!dll) return false;
        create   = (create_fn)  GetProcAddress(dll, "xi_plugin_create");
        destroy  = (destroy_fn) GetProcAddress(dll, "xi_plugin_destroy");
        process  = (process_fn) GetProcAddress(dll, "xi_plugin_process");
        exchange = (exchange_fn)GetProcAddress(dll, "xi_plugin_exchange");
        get_def  = (get_def_fn) GetProcAddress(dll, "xi_plugin_get_def");
        set_def  = (set_def_fn) GetProcAddress(dll, "xi_plugin_set_def");
        return create && destroy;
    }
};

int main(int argc, char** argv) {
    // SEH translator at thread entry so any plugin segfault becomes a
    // C++ exception we can catch + report cleanly to the backend.
    xi::install_seh_translator();

    std::string pipe_name   = arg_get(argc, argv, "pipe");
    std::string shm_name    = arg_get(argc, argv, "shm");
    std::string plugin_dll  = arg_get(argc, argv, "plugin-dll");
    std::string instance    = arg_get(argc, argv, "instance", "worker0");
    std::string inst_folder = arg_get(argc, argv, "instance-folder");

    if (pipe_name.empty() || shm_name.empty() || plugin_dll.empty()) {
        std::fprintf(stderr,
            "usage: xinsp-worker --pipe=NAME --shm=NAME --plugin-dll=PATH "
            "[--instance=NAME] [--instance-folder=PATH]\n");
        return 2;
    }

    std::fprintf(stderr,
        "[worker] pipe=%s shm=%s dll=%s instance=%s\n",
        pipe_name.c_str(), shm_name.c_str(), plugin_dll.c_str(), instance.c_str());

    // Register the instance folder so host->instance_folder() inside
    // the worker (and Plugin::folder_path()) returns the same path the
    // backend would. Otherwise the plugin sees an empty string under
    // isolation and any "save calibration / load template" code breaks.
    if (!inst_folder.empty()) {
        xi::InstanceFolderRegistry::instance().set(instance, inst_folder);
    }

    // Attach the backend's SHM region. After this every image_data() in
    // host_api will return a pointer into shared memory — completely
    // transparent to the plugin code.
    std::unique_ptr<xi::ShmRegion> shm;
    try {
        shm = std::make_unique<xi::ShmRegion>(xi::ShmRegion::attach(shm_name));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[worker] shm attach failed: %s\n", e.what());
        return 3;
    }
    xi::ImagePool::set_shm_region(shm.get());
    // Switch xi::Image::create_in_pool / Plugin::pool_image to use
    // host->shm_create_image so plugin outputs land in SHM directly.
    // Without this they'd go to the worker's local heap pool and the
    // RPC reply path would have to memcpy them into SHM at the boundary.
    xi::set_worker_mode(true);
    std::fprintf(stderr, "[worker] shm attached, %lluMB visible\n",
                 (unsigned long long)(shm->total_size() / (1024 * 1024)));

    // Build a host_api for the plugin. Heap-pool fns still work — they
    // hit the worker's local ImagePool, which is fine for any image the
    // plugin creates only for its own use. But normally the plugin will
    // use shm_create_image so the backend can see the result.
    static xi_host_api host = xi::ImagePool::make_host_api();
    // emit_trigger from inside an isolated worker pushes the call back
    // to the backend's TriggerBus singleton via a one-way frame
    // (RPC_EMIT_TRIGGER, seq=0, no reply expected). This lets source
    // plugins run isolated and still drive multi-camera correlation.
    //
    // Concurrency: the source plugin's worker thread calls this lambda
    // while the worker's main loop may be in the middle of replying to
    // a different RPC (or blocked in recv_frame waiting for the next
    // request). Win32 named pipes allow concurrent read+write on the
    // same handle, but two writers (main loop sending replies, plugin
    // thread sending async events) must serialise — `g_pipe_write_mu`
    // below covers that.
    //
    // Image handles in the payload need to be SHM-tagged so the
    // backend's bus can deref them through host_api. Heap-pool handles
    // (the worker's local pool) are useless across the process
    // boundary — promote them to SHM here, same logic that
    // promote_to_shm uses for process replies.
    g_host_for_async = host;  // capture host_api fns for the async lambda
    host.emit_trigger = [](const char* source, xi_trigger_id tid,
                           int64_t ts_us,
                           const xi_record_image* images, int32_t n) {
        if (!g_pipe_for_async) return;
        // Promote any non-SHM handles to SHM before sending — the
        // backend can't dereference worker-local heap-pool handles.
        std::vector<xi_image_handle> temp_shm;
        std::vector<xi_record_image> promoted;
        promoted.reserve((size_t)n);
        for (int32_t i = 0; i < n; ++i) {
            xi_image_handle h = images[i].handle;
            if (h && !g_host_for_async.shm_is_shm_handle(h)) {
                int w  = g_host_for_async.image_width(h);
                int hh = g_host_for_async.image_height(h);
                int ch = g_host_for_async.image_channels(h);
                int s  = g_host_for_async.image_stride(h);
                const uint8_t* src = g_host_for_async.image_data(h);
                if (src && w > 0 && hh > 0 && ch > 0) {
                    xi_image_handle shm_h = g_host_for_async.shm_create_image(w, hh, ch);
                    if (shm_h) {
                        uint8_t* dst = g_host_for_async.image_data(shm_h);
                        int row = w * ch;
                        int src_stride = s > 0 ? s : row;
                        for (int y = 0; y < hh; ++y)
                            std::memcpy(dst + y * row, src + y * src_stride, (size_t)row);
                        h = shm_h;
                        temp_shm.push_back(shm_h);
                    }
                }
            }
            xi_record_image rec{};
            rec.key    = images[i].key;
            rec.handle = h;
            promoted.push_back(rec);
        }
        ipc::Writer w;
        w.bytes(source ? source : "", source ? std::strlen(source) : 0);
        w.u64(tid.hi);
        w.u64(tid.lo);
        w.u64((uint64_t)ts_us);
        w.u32((uint32_t)promoted.size());
        for (auto& rec : promoted) {
            w.bytes(rec.key ? rec.key : "", rec.key ? std::strlen(rec.key) : 0);
            w.u64(rec.handle);
        }
        try {
            std::lock_guard<std::mutex> lk(g_pipe_write_mu);
            ipc::send_frame(*g_pipe_for_async, /*seq=*/0,
                            ipc::RPC_EMIT_TRIGGER,
                            w.buf().data(), (uint32_t)w.buf().size());
        } catch (const std::exception& e) {
            std::fprintf(stderr,
                "[worker] emit_trigger send failed for '%s': %s\n",
                source ? source : "?", e.what());
        }
        // Release the temp SHM handles — we only needed them to ferry
        // the bytes; the bus addrefs each handle on emit, the original
        // refcount==1 we're holding can drop.
        for (auto h : temp_shm) g_host_for_async.image_release(h);
    };

    // Connect to the backend's pipe. Backend should already have the
    // server side waiting.
    ipc::Pipe pipe;
    try {
        pipe = ipc::Pipe::connect(pipe_name);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[worker] pipe connect failed: %s\n", e.what());
        return 4;
    }
    std::fprintf(stderr, "[worker] pipe connected\n");
    // Publish the pipe to the async-write path (emit_trigger lambda).
    g_pipe_for_async = &pipe;

    // Load the plugin DLL and let the first CREATE request instantiate it.
    PluginProxy plugin;
    if (!plugin.load(plugin_dll)) {
        std::fprintf(stderr, "[worker] failed to load %s or resolve entry points\n",
                     plugin_dll.c_str());
        return 5;
    }

    // RPC service loop. ONE plugin instance per worker; CREATE allocates,
    // DESTROY frees + exits, anything else needs the instance to exist.
    auto require_inst = [&](uint32_t seq) -> bool {
        if (plugin.inst) return true;
        mu_send_error_reply(pipe, seq, "no instance — call CREATE first");
        return false;
    };

    while (true) {
        ipc::Frame f;
        try { f = ipc::recv_frame(pipe); }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[worker] pipe closed: %s\n", e.what());
            break;
        }

        try {
            switch (f.type) {
            case ipc::RPC_CREATE: {
                ipc::Reader r(f.payload);
                std::string name    = r.str();
                std::string dll_arg = r.str();   // currently unused; plugin already loaded
                (void)dll_arg;
                if (plugin.inst) plugin.destroy(plugin.inst);
                plugin.inst = plugin.create(&host, name.empty() ? instance.c_str() : name.c_str());
                if (!plugin.inst) {
                    mu_send_error_reply(pipe, f.seq, "plugin create returned null");
                    break;
                }
                ipc::Writer w; w.u8(1);
                mu_send_reply(pipe, f.seq, ipc::RPC_CREATE,
                                w.buf().data(), (uint32_t)w.buf().size());
                std::fprintf(stderr, "[worker] CREATE ok\n");
                break;
            }
            case ipc::RPC_DESTROY: {
                if (plugin.inst) { plugin.destroy(plugin.inst); plugin.inst = nullptr; }
                ipc::Writer w; w.u8(1);
                mu_send_reply(pipe, f.seq, ipc::RPC_DESTROY,
                                w.buf().data(), (uint32_t)w.buf().size());
                std::fprintf(stderr, "[worker] DESTROY ok, exiting\n");
                return 0;
            }
            case ipc::RPC_PROCESS: {
                if (!require_inst(f.seq)) break;
                if (!plugin.process) {
                    mu_send_error_reply(pipe, f.seq, "plugin has no process()");
                    break;
                }
                // Wire format (matches ProcessInstanceAdapter::process_via_rpc):
                //   in : u32 count | for each (str key | u64 handle) | bytes json
                //   out: u32 count | for each (str key | u64 handle) | bytes json
                ipc::Reader r(f.payload);
                uint32_t n_in = r.u32();
                std::vector<std::string>     in_keys(n_in);
                std::vector<xi_record_image> in_imgs(n_in);
                for (uint32_t i = 0; i < n_in; ++i) {
                    in_keys[i] = r.str();
                    in_imgs[i].key    = in_keys[i].c_str();
                    in_imgs[i].handle = r.u64();
                }
                std::vector<uint8_t> in_json = r.bytes();

                xi_record in_rec{};
                in_rec.images      = n_in ? in_imgs.data() : nullptr;
                in_rec.image_count = (int)n_in;
                std::string in_str(in_json.begin(), in_json.end());
                in_rec.json        = in_str.c_str();

                xi_record_out out_rec{};
                plugin.process(plugin.inst, &in_rec, &out_rec);

                // Heap-pool handles (no 0xA5 tag) point into the worker's
                // local ImagePool — useless to the backend. Auto-convert
                // every output image to SHM so plugin authors don't have
                // to know which mode they're running in.
                auto promote_to_shm = [&](xi_image_handle h) -> xi_image_handle {
                    if (!h || host.shm_is_shm_handle(h)) return h;
                    int w_ = host.image_width(h);
                    int h_ = host.image_height(h);
                    int c_ = host.image_channels(h);
                    int s_ = host.image_stride(h);
                    const uint8_t* src = host.image_data(h);
                    if (!src || w_ <= 0 || h_ <= 0 || c_ <= 0) return h;
                    xi_image_handle shm_h = host.shm_create_image(w_, h_, c_);
                    if (!shm_h) return h;
                    uint8_t* dst = const_cast<uint8_t*>(host.image_data(shm_h));
                    int row_bytes = w_ * c_;
                    for (int y = 0; y < h_; ++y)
                        std::memcpy(dst + y * row_bytes,
                                    src + y * (s_ > 0 ? s_ : row_bytes),
                                    (size_t)row_bytes);
                    host.image_release(h);
                    return shm_h;
                };

                ipc::Writer w;
                uint32_t n_out = (uint32_t)out_rec.image_count;
                w.u32(n_out);
                for (uint32_t i = 0; i < n_out; ++i) {
                    auto& img = out_rec.images[i];
                    const char* k = img.key ? img.key : "";
                    w.str(std::string(k));
                    w.u64(promote_to_shm(img.handle));
                }
                std::string out_json = out_rec.json ? out_rec.json : "{}";
                w.bytes(out_json.data(), out_json.size());
                mu_send_reply(pipe, f.seq, ipc::RPC_PROCESS,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            case ipc::RPC_EXCHANGE: {
                if (!require_inst(f.seq)) break;
                if (!plugin.exchange) {
                    mu_send_error_reply(pipe, f.seq, "plugin has no exchange()");
                    break;
                }
                ipc::Reader r(f.payload);
                std::string cmd = r.str();
                std::vector<char> rsp(64 * 1024);
                int n = plugin.exchange(plugin.inst, cmd.c_str(),
                                        rsp.data(), (int)rsp.size());
                if (n < 0) { rsp.resize((size_t)(-n) + 1024);
                             n = plugin.exchange(plugin.inst, cmd.c_str(),
                                                 rsp.data(), (int)rsp.size()); }
                ipc::Writer w;
                if (n > 0) w.bytes(rsp.data(), (size_t)n);
                else       w.bytes("", 0);
                mu_send_reply(pipe, f.seq, ipc::RPC_EXCHANGE,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            case ipc::RPC_GET_DEF: {
                if (!require_inst(f.seq)) break;
                if (!plugin.get_def) {
                    mu_send_error_reply(pipe, f.seq, "plugin has no get_def()");
                    break;
                }
                std::vector<char> buf(4096);
                int n = plugin.get_def(plugin.inst, buf.data(), (int)buf.size());
                if (n < 0) { buf.resize((size_t)(-n) + 1024);
                             n = plugin.get_def(plugin.inst, buf.data(), (int)buf.size()); }
                ipc::Writer w;
                w.bytes(buf.data(), (size_t)(n > 0 ? n : 0));
                mu_send_reply(pipe, f.seq, ipc::RPC_GET_DEF,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            case ipc::RPC_SET_DEF: {
                if (!require_inst(f.seq)) break;
                if (!plugin.set_def) {
                    mu_send_error_reply(pipe, f.seq, "plugin has no set_def()");
                    break;
                }
                ipc::Reader r(f.payload);
                std::string def = r.str();
                int rc = plugin.set_def(plugin.inst, def.c_str());
                ipc::Writer w; w.u8(rc == 0 ? 1 : 0);
                mu_send_reply(pipe, f.seq, ipc::RPC_SET_DEF,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            default:
                mu_send_error_reply(pipe, f.seq,
                    "unknown rpc type " + std::to_string(f.type));
                break;
            }
        } catch (const xi::seh_exception& e) {
            std::fprintf(stderr, "[worker] plugin SEH: 0x%08X (%s)\n", e.code, e.what());
            mu_send_error_reply(pipe, f.seq,
                std::string("plugin crashed: ") + e.what());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[worker] exception: %s\n", e.what());
            mu_send_error_reply(pipe, f.seq, e.what());
        }
    }

    if (plugin.inst) plugin.destroy(plugin.inst);
    if (plugin.dll)  FreeLibrary(plugin.dll);
    return 0;
}
