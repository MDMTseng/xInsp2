#pragma once
//
// xi_thread.hpp — std::thread wrapper that installs SEH translation and
// catches stray exceptions at thread entry.
//
//     auto t = xi::spawn_worker("camera-poll", [&]{ run_poll_loop(); });
//
// Equivalent to std::thread except the body is wrapped:
//   1. _set_se_translator(seh_translator) — SEH becomes seh_exception
//   2. try/catch around the body — logs and swallows so the process
//      doesn't terminate when a worker thread throws
//
// Plugin authors should prefer this over raw std::thread for any thread
// they spawn from inside the plugin DLL: a stray null-deref on a worker
// thread without an installed translator brings down the whole backend.
//
// `xi::async` (xi_async.hpp) applies the same wrapping for std::async tasks.
//

#include "xi_seh.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace xi {

template <class F, class... Args>
std::thread spawn_worker(std::string name, F&& f, Args&&... args) {
    auto closure =
        [name = std::move(name),
         fn   = std::forward<F>(f),
         tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
            xi::install_seh_translator();
            try {
                std::apply(std::move(fn), std::move(tup));
            } catch (const xi::seh_exception& e) {
                std::fprintf(stderr,
                    "[xinsp2] worker '%s' crashed: 0x%08X (%s)\n",
                    name.c_str(), e.code, e.what());
            } catch (const std::exception& e) {
                std::fprintf(stderr,
                    "[xinsp2] worker '%s' threw: %s\n",
                    name.c_str(), e.what());
            } catch (...) {
                std::fprintf(stderr,
                    "[xinsp2] worker '%s' threw unknown exception\n",
                    name.c_str());
            }
        };
    return std::thread(std::move(closure));
}

} // namespace xi
