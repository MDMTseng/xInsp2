//
// script_runner_main.cpp — `xinsp-script-runner.exe` entry point.
//
// Phase 3 minimum: hosts ONE inspection script DLL in its own process,
// attached to the backend's SHM region. Backend drives it via:
//
//   RPC_SCRIPT_RUN(frame_num) → reset script state → call xi_inspect_entry
//                              → snapshot ValueStore → reply with vars JSON
//
// What's deliberately skipped (deferred to Phase 3.5):
//   - use_* callback proxying back to backend (script can only allocate
//     host_api images and set local Vars; calling xi::use<T> in
//     isolated mode does nothing useful)
//   - trigger / breakpoint / params / state / list_instances proxying
//   - bidirectional RPC during inspect (script→backend mid-call)
//
// Script DLL must export at least:
//   xi_inspect_entry(int frame)
//   xi_script_reset()           [optional; if missing we no-op]
//   xi_script_snapshot_vars(buf, len)
//
// Crashes here don't kill the backend. Phase 2.7's auto-respawn
// pattern would extend cleanly to a future ScriptProcessAdapter.
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
#include <string>

#include <xi/xi_image_pool.hpp>
#include <xi/xi_ipc.hpp>
#include <xi/xi_seh.hpp>
#include <xi/xi_shm.hpp>

namespace ipc = xi::ipc;

static std::string arg_get(int argc, char** argv, const std::string& key,
                           const std::string& def = "") {
    std::string prefix = "--" + key + "=";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return def;
}

int main(int argc, char** argv) {
    xi::install_seh_translator();

    std::string pipe_name  = arg_get(argc, argv, "pipe");
    std::string shm_name   = arg_get(argc, argv, "shm");
    std::string script_dll = arg_get(argc, argv, "script-dll");
    if (pipe_name.empty() || shm_name.empty() || script_dll.empty()) {
        std::fprintf(stderr,
            "usage: xinsp-script-runner --pipe=NAME --shm=NAME --script-dll=PATH\n");
        return 2;
    }

    std::fprintf(stderr,
        "[script-runner] pipe=%s shm=%s dll=%s\n",
        pipe_name.c_str(), shm_name.c_str(), script_dll.c_str());

    // Attach backend's SHM region. Any image the script allocates via
    // host_api->shm_create_image will live here, visible to backend
    // immediately and zero-copy.
    std::unique_ptr<xi::ShmRegion> shm;
    try {
        shm = std::make_unique<xi::ShmRegion>(xi::ShmRegion::attach(shm_name));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[script-runner] shm attach failed: %s\n", e.what());
        return 3;
    }
    xi::ImagePool::set_shm_region(shm.get());

    // host_api is configured but use_* fn pointers stay null in this
    // minimum slice — the script gets image_create / shm_create_image
    // / image_data etc. but xi::use<T> calls will no-op.
    static xi_host_api host = xi::ImagePool::make_host_api();
    (void)host;

    // Connect to the backend's pipe.
    ipc::Pipe pipe;
    try { pipe = ipc::Pipe::connect(pipe_name); }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[script-runner] pipe connect failed: %s\n", e.what());
        return 4;
    }

    HMODULE dll = LoadLibraryA(script_dll.c_str());
    if (!dll) {
        std::fprintf(stderr, "[script-runner] LoadLibrary failed: %lu\n", GetLastError());
        return 5;
    }

    using inspect_fn  = void (*)(int);
    using reset_fn    = void (*)();
    using snapshot_fn = int  (*)(char*, int);

    auto inspect = (inspect_fn) GetProcAddress(dll, "xi_inspect_entry");
    auto reset   = (reset_fn)   GetProcAddress(dll, "xi_script_reset");
    auto snap    = (snapshot_fn)GetProcAddress(dll, "xi_script_snapshot_vars");
    if (!inspect) {
        std::fprintf(stderr, "[script-runner] script missing xi_inspect_entry\n");
        return 6;
    }
    std::fprintf(stderr, "[script-runner] script loaded; inspect=%p reset=%p snap=%p\n",
                 inspect, reset, snap);

    // RPC loop. Each request: reset → inspect_entry → snapshot → reply.
    while (true) {
        ipc::Frame f;
        try { f = ipc::recv_frame(pipe); }
        catch (...) { std::fprintf(stderr, "[script-runner] pipe closed\n"); break; }

        try {
            switch (f.type) {
            case ipc::RPC_SCRIPT_RUN: {
                ipc::Reader r(f.payload);
                uint32_t frame = r.u32();
                if (reset) reset();
                inspect((int)frame);
                std::vector<char> vbuf(64 * 1024);
                int n = snap ? snap(vbuf.data(), (int)vbuf.size()) : 0;
                if (n < 0) {
                    vbuf.resize((size_t)(-n) + 1024);
                    n = snap(vbuf.data(), (int)vbuf.size());
                }
                ipc::Writer w;
                if (n > 0) w.bytes(vbuf.data(), (size_t)n);
                else       w.bytes("[]", 2);
                ipc::send_reply(pipe, f.seq, ipc::RPC_SCRIPT_RUN,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            case ipc::RPC_DESTROY: {
                ipc::Writer w; w.u8(1);
                ipc::send_reply(pipe, f.seq, ipc::RPC_DESTROY,
                                w.buf().data(), (uint32_t)w.buf().size());
                FreeLibrary(dll);
                return 0;
            }
            default:
                ipc::send_error_reply(pipe, f.seq,
                    "script-runner: unsupported rpc " + std::to_string(f.type));
                break;
            }
        } catch (const xi::seh_exception& e) {
            std::fprintf(stderr, "[script-runner] script SEH: 0x%08X (%s)\n",
                         e.code, e.what());
            ipc::send_error_reply(pipe, f.seq,
                std::string("script crashed: ") + e.what());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[script-runner] exception: %s\n", e.what());
            ipc::send_error_reply(pipe, f.seq, e.what());
        }
    }

    FreeLibrary(dll);
    return 0;
}
