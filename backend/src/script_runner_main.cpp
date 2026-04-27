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

    // Connect to the backend's pipe and wrap in a Session for
    // bidirectional RPC. The session lets the runner send a request
    // (RPC_USE_PROCESS) to the backend mid-inspect and have the
    // reply routed correctly even though the backend is currently
    // waiting for our RPC_SCRIPT_RUN reply.
    std::unique_ptr<ipc::Session> sess_ptr;
    try {
        sess_ptr = std::make_unique<ipc::Session>(ipc::Pipe::connect(pipe_name));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[script-runner] pipe connect failed: %s\n", e.what());
        return 4;
    }
    auto& sess = *sess_ptr;

    HMODULE dll = LoadLibraryA(script_dll.c_str());
    if (!dll) {
        std::fprintf(stderr, "[script-runner] LoadLibrary failed: %lu\n", GetLastError());
        return 5;
    }

    using inspect_fn  = void (*)(int);
    using reset_fn    = void (*)();
    using snapshot_fn = int  (*)(char*, int);
    using set_cb_fn   = void (*)(void*, void*, void*, void*);
    using set_input_fn= void (*)(uint64_t);

    auto inspect    = (inspect_fn) GetProcAddress(dll, "xi_inspect_entry");
    auto reset      = (reset_fn)   GetProcAddress(dll, "xi_script_reset");
    auto snap       = (snapshot_fn)GetProcAddress(dll, "xi_script_snapshot_vars");
    auto set_cb     = (set_cb_fn)  GetProcAddress(dll, "xi_script_set_use_callbacks");
    auto set_input  = (set_input_fn)GetProcAddress(dll, "xi_test_set_input");
    if (!inspect) {
        std::fprintf(stderr, "[script-runner] script missing xi_inspect_entry\n");
        return 6;
    }
    std::fprintf(stderr,
        "[script-runner] script loaded; inspect=%p reset=%p snap=%p set_cb=%p set_in=%p\n",
        inspect, reset, snap, set_cb, set_input);

    // ---- use_* callback proxy ----
    //
    // The script calls these via fn pointers it received from
    // xi_script_set_use_callbacks. We install runner-local impls that
    // hop the request back to the backend over the same Session pipe.
    //
    // Stored as a global because the script ABI's set_use_callbacks
    // takes void* fn pointers — there's no userdata channel for us
    // to thread the Session through.
    static ipc::Session* g_sess = &sess;

    // use_process(name, input_json, input_images, n, &output_record)
    auto runner_use_process = [](const char* name,
                                 const char* input_json,
                                 const xi_record_image* input_images,
                                 int input_image_count,
                                 xi_record_out* output) -> int {
        if (!g_sess || !name) return -1;
        // Phase 3.5 minimum: only forward the FIRST input image. Multi-
        // image inputs are easy to extend later; the wire format reserves
        // room with json_len prefix etc.
        ipc::Writer w;
        w.str(name);
        w.u64((input_image_count > 0 && input_images) ? input_images[0].handle : 0);
        const char* j = input_json ? input_json : "{}";
        w.bytes(j, std::strlen(j));
        try {
            auto rsp = g_sess->call(ipc::RPC_USE_PROCESS, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) {
                std::fprintf(stderr, "[script-runner] use_process error: %.*s\n",
                             (int)rsp.payload.size(), rsp.payload.data());
                return -2;
            }
            ipc::Reader r(rsp.payload);
            uint64_t out_h = r.u64();
            auto out_json_bytes = r.bytes();
            // Stash in a per-call thread-local so xi_record_out's
            // pointer-fields stay valid until the caller copies them.
            static thread_local xi_record_image s_out_img;
            static thread_local std::string     s_out_json;
            s_out_img.key = "out";
            s_out_img.handle = out_h;
            s_out_json.assign(out_json_bytes.begin(), out_json_bytes.end());
            output->images       = (out_h ? &s_out_img : nullptr);
            output->image_count  = (out_h ? 1 : 0);
            output->json         = s_out_json.data();
            return output->image_count;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[script-runner] use_process threw: %s\n", e.what());
            return -3;
        }
    };

    // use_exchange(name, cmd_json, rsp_buf, rsp_len) → bytes written or -needed.
    auto runner_use_exchange = [](const char* name, const char* cmd,
                                   char* rsp_buf, int rsp_buflen) -> int {
        if (!g_sess || !name) return -1;
        ipc::Writer w;
        w.str(name);
        w.str(cmd ? cmd : "");
        try {
            auto rsp = g_sess->call(ipc::RPC_USE_EXCHANGE, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) return -1;
            ipc::Reader r(rsp.payload);
            auto bytes = r.bytes();
            int n = (int)bytes.size();
            if (rsp_buflen < n + 1) return -n;
            std::memcpy(rsp_buf, bytes.data(), (size_t)n);
            rsp_buf[n] = 0;
            return n;
        } catch (...) { return -1; }
    };

    // use_grab(name, timeout_ms) → image handle.
    auto runner_use_grab = [](const char* name, int timeout_ms) -> xi_image_handle {
        if (!g_sess || !name) return XI_IMAGE_NULL;
        ipc::Writer w;
        w.str(name);
        w.u32((uint32_t)timeout_ms);
        try {
            auto rsp = g_sess->call(ipc::RPC_USE_GRAB, w.buf());
            if (rsp.type == ipc::RPC_TYPE_ERROR) return XI_IMAGE_NULL;
            ipc::Reader r(rsp.payload);
            return r.u64();
        } catch (...) { return XI_IMAGE_NULL; }
    };

    // Install callbacks on the script. Plus the script needs the
    // host_api so it can deref input image bytes locally (SHM).
    if (set_cb) {
        set_cb((void*)+runner_use_process,
               (void*)+runner_use_exchange,
               (void*)+runner_use_grab,
               (void*)&host);
    }

    // Backend-side dispatch isn't needed in the runner: the only
    // requests it RECEIVES are the ones below. Its handler stays empty
    // (nothing reverse-dispatches into the script). Sessions used in a
    // pure server role go through serve_forever, but here we want the
    // explicit type switch + reset+inspect+snapshot orchestration so we
    // walk frames manually.
    auto& pipe = sess.pipe();
    while (true) {
        ipc::Frame f;
        try { f = ipc::recv_frame(pipe); }
        catch (...) { std::fprintf(stderr, "[script-runner] pipe closed\n"); break; }

        // Note: this raw recv is OK because we're the side that ONLY
        // receives requests at the top level. Any USE_PROCESS the
        // script triggers happens INSIDE a request's call to inspect()
        // and goes through sess.call() which has its own loop.
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
                ipc::send_reply(pipe,
                                f.seq, ipc::RPC_SCRIPT_RUN,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            case ipc::RPC_TEST_SET_INPUT: {
                ipc::Reader r(f.payload);
                uint64_t h = r.u64();
                if (set_input) set_input(h);
                ipc::Writer w; w.u8(1);
                ipc::send_reply(pipe,
                                f.seq, ipc::RPC_TEST_SET_INPUT,
                                w.buf().data(), (uint32_t)w.buf().size());
                break;
            }
            case ipc::RPC_DESTROY: {
                ipc::Writer w; w.u8(1);
                ipc::send_reply(pipe,
                                f.seq, ipc::RPC_DESTROY,
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
