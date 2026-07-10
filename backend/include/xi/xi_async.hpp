#pragma once
//
// xi_async.hpp — parallelism primitives for xInsp2 inspection routines.
//
// User-facing surface (four names):
//
//   xi::async(fn, args...)        -> Future<R>   spawn a task
//   Future<T> implicit to T                       blocks, re-throws exceptions
//   xi::await_all(f1, f2, ...)                    tuple of results
//   ASYNC_WRAP(name)                              declare async_<name>(args...)
//
// Example:
//
//   auto p1 = xi::async(featureA, gray);
//   auto p2 = xi::async(featureB, gray);
//   Image a = p1;                 // await
//   Image b = p2;                 // await
//
// Backed by std::async(launch::async) initially. Swap for a thread pool later
// without touching user code — the API is stable.
//

#include "xi_seh.hpp"
#include "xi_crash_dump.hpp"   // xi::crash::reserve_fault_stack — 128 KB dump headroom

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

// Image-pool owner get/set thunks, injected by the host into the script DLL via
// xi_script_set_owner_callbacks (see C1). xi::async (C2) and xi::parallel_for
// (C3) carry the inspect-thread owner onto the worker threads they spawn by
// reading/writing these. Null on an older host / non-script TU ⇒ the wrappers
// below are silent no-ops and worker-created images stay anonymous (owner=0),
// exactly as before.
//
// These are `inline` (one shared, zero-initialised definition per module) rather
// than the `extern`-here / `static`-in-xi_script_support.hpp idiom used for the
// g_use_*/g_trigger_* globals. That idiom only works because those globals are
// ODR-used EXCLUSIVELY from script TUs (which include xi_script_support.hpp's
// static definitions). The owner thunks differ: detail::owner_get() below is
// ODR-used UNCONDITIONALLY from xi::async / xi::parallel_for, which NON-script
// TUs instantiate too (unit tests, a plugin op-library using xi::async). An
// `extern` with no definition reachable from those TUs is an unresolved-external
// link error. `inline` gives every such TU its own zero-initialised copy and the
// exported setter writes the script DLL's copy that the script's async reads —
// same module, so it stays consistent. (Global scope to match the setter, which
// writes ::g_owner_*_fn_.)
inline void* g_owner_get_fn_ = nullptr;
inline void* g_owner_set_fn_ = nullptr;

// A4 explicit per-run context get/set thunks, injected by the host via
// xi_script_set_run_ctx_callbacks. xi::async / xi::parallel_for / xi::spawn_worker
// carry the inspect thread's per-run context (run_id + frame_path + verdict slot +
// dispatch-thread identity, an opaque host RunContext*) onto the worker threads
// they spawn by reading/writing these — so xi::run_id() / xi::current_frame_path()
// / xi::result() are correct on a worker (closing the spawn gap), and the host's
// off-thread read/push detection rides that same context (owner_tid / had_trigger)
// instead of a separate marker. Null on an older host / non-script TU ⇒ the
// wrappers below are silent no-ops (get() returns null, set() does nothing) and
// the context simply doesn't propagate, exactly as before. Same `inline`
// rationale as the owner thunks above (ODR-used unconditionally from generic
// async/parallel_for code).
inline void* g_run_ctx_get_fn_ = nullptr;
inline void* g_run_ctx_set_fn_ = nullptr;
// xi::spawn_worker BY-VALUE path: a fire-and-forget worker may OUTLIVE the inspect,
// so it must not hold a pointer into the spawning frame (that would dangle). These
// let it install a worker-OWNED heap snapshot instead — snapshot allocates a copy of
// the current context on the spawning thread (result_slot nulled host-side), install
// stamps the worker's own thread id and installs it, free releases it on worker exit.
// Null on an older host / non-script TU ⇒ spawn_worker simply propagates no context.
inline void* g_run_ctx_snapshot_fn_       = nullptr;
inline void* g_run_ctx_install_worker_fn_ = nullptr;
inline void* g_run_ctx_free_fn_           = nullptr;

namespace xi {

namespace detail {
using OwnerGetFn = uint32_t (*)();
using OwnerSetFn = void     (*)(uint32_t);

// Read the image-pool owner active on THIS thread (0 if no host wired the thunk).
inline uint32_t owner_get() {
    auto fn = reinterpret_cast<OwnerGetFn>(::g_owner_get_fn_);
    return fn ? fn() : 0u;
}
// Install `id` as the image-pool owner on THIS thread (no-op if unwired).
inline void owner_set(uint32_t id) {
    auto fn = reinterpret_cast<OwnerSetFn>(::g_owner_set_fn_);
    if (fn) fn(id);
}

// A4: read/write THIS thread's per-run context pointer (null if no host wired it).
// The pointer is opaque to the SDK (an internal host RunContext*); we only marshal
// it across the ABI seam so a worker inherits the run and the host's accessors
// (run_id / frame_path / result / off-thread detection) resolve it host-side.
using RunCtxGetFn = const void* (*)();
using RunCtxSetFn = void        (*)(const void*);
inline const void* run_ctx_get() {
    auto fn = reinterpret_cast<RunCtxGetFn>(::g_run_ctx_get_fn_);
    return fn ? fn() : nullptr;
}
inline void run_ctx_set(const void* p) {
    auto fn = reinterpret_cast<RunCtxSetFn>(::g_run_ctx_set_fn_);
    if (fn) fn(p);
}

// RAII: install `p` as this thread's per-run context for the scope, restore the
// previous pointer on exit. Symmetric with OwnerScope — used by xi::async /
// xi::parallel_for / xi::spawn_worker so a worker inherits the spawning inspect's
// run, making xi::run_id() / xi::current_frame_path() / xi::result() correct on
// the worker and the host's off-thread read/push detection accurate.
struct RunCtxScope {
    const void* prev;
    explicit RunCtxScope(const void* p) : prev(run_ctx_get()) { run_ctx_set(p); }
    ~RunCtxScope() { run_ctx_set(prev); }
    RunCtxScope(const RunCtxScope&) = delete;
    RunCtxScope& operator=(const RunCtxScope&) = delete;
};

// spawn_worker BY-VALUE snapshot thunks (see the g_run_ctx_snapshot_fn_ note).
using RunCtxSnapshotFn      = void* (*)();
using RunCtxInstallWorkerFn = void  (*)(void*);
using RunCtxFreeFn          = void  (*)(void*);
inline void* run_ctx_snapshot() {
    auto fn = reinterpret_cast<RunCtxSnapshotFn>(::g_run_ctx_snapshot_fn_);
    return fn ? fn() : nullptr;
}
inline void run_ctx_install_worker(void* s) {
    auto fn = reinterpret_cast<RunCtxInstallWorkerFn>(::g_run_ctx_install_worker_fn_);
    if (fn) fn(s);
}
inline void run_ctx_free(void* s) {
    auto fn = reinterpret_cast<RunCtxFreeFn>(::g_run_ctx_free_fn_);
    if (fn) fn(s);
}

// RAII for xi::spawn_worker: install a worker-OWNED snapshot (taken on the SPAWNING
// thread, passed in by value) and free it on exit. NO pointer into the spawning
// frame — structurally UAF-free even if the worker outlives the inspect. `snap` may
// be null (spawned off a run / older host): install(null) clears the context and
// free(null) is a no-op.
struct WorkerRunCtxScope {
    void*       snap;
    const void* prev;
    explicit WorkerRunCtxScope(void* s) : snap(s), prev(run_ctx_get()) { run_ctx_install_worker(s); }
    ~WorkerRunCtxScope() { run_ctx_set(prev); run_ctx_free(snap); }
    WorkerRunCtxScope(const WorkerRunCtxScope&) = delete;
    WorkerRunCtxScope& operator=(const WorkerRunCtxScope&) = delete;
};

// RAII: install `id` as this thread's image-pool owner for the scope, restore the
// previous owner on exit. Symmetric with the cancel-token Scope — used by
// xi::async and xi::parallel_for to attribute worker-created images.
struct OwnerScope {
    uint32_t prev;
    explicit OwnerScope(uint32_t id) : prev(owner_get()) { owner_set(id); }
    ~OwnerScope() { owner_set(prev); }
    OwnerScope(const OwnerScope&) = delete;
    OwnerScope& operator=(const OwnerScope&) = delete;
};
} // namespace detail

// Cancellation token. Shared between a Future<T> and the worker
// task it spawned. Caller-side `cancel()` flips the bit; worker-
// side `is_cancelled()` polls. Cooperation is required — a long-
// running op that never polls won't notice.
//
// Long-running plugin / script code should call
// `xi::cancellation_requested()` (below) at loop heads / on chunk
// boundaries to make itself abort-friendly. cv:: calls cannot be
// cancelled mid-op; structure work into chunks and poll between them
// if cancellation responsiveness matters.
//
// TODO(watchdog): hook this up so cmd:run's watchdog cancels all
// outstanding tokens before falling back to TerminateThread. Lets a
// stuck script abort cleanly when its ops are cooperative, before we
// reach for the unsafe kill primitive.
struct CancelToken {
    std::atomic<bool> cancelled{false};
};
using CancelTokenPtr = std::shared_ptr<CancelToken>;

// Per-thread current token. Set by `xi::async` before the user
// callable runs; readable by `cancellation_requested()` from
// anywhere on the worker thread (including ops nested inside the
// user callable).
inline CancelToken*& current_cancel_token_ref() {
    static thread_local CancelToken* p = nullptr;
    return p;
}

// `cancellation_requested()` — cooperative-cancel poll for long-running plugin
// / script code. Reads ONLY the per-task xi::async cancel token installed on
// this thread (by xi::async before the user callable runs; propagated into
// xi::parallel_for workers). Returns true once that token is flipped — via
// Future::cancel() or a dropped-unconsumed Future. Cheap: one relaxed pointer
// load plus, only when a token is present, one relaxed bool load — no locks.
//
// [retired] The watchdog's cooperative EPOCH-cancel (a monotonic inspect-ticket
// counter + arm/clear cutoff that soft-cancelled in-flight inspects) was removed
// along with the whole soft-cancel layer. A wedged inspect now runs until the
// watchdog's HARD trip (_Exit + FE respawn, see service_main.cpp) — there is no
// soft-cancel state left to poll here.
inline bool cancellation_requested() {
    auto* t = current_cancel_token_ref();
    return t && t->cancelled.load(std::memory_order_relaxed);
}

template <class T>
class Future {
public:
    Future() = default;
    Future(std::future<T>&& f, CancelTokenPtr tok)
        : f_(std::move(f)), token_(std::move(tok)) {}

    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    // Dropping an unconsumed Future: cooperatively cancel the sibling task
    // BEFORE the std::future member's dtor blocks on it, so a task that polls
    // `xi::cancellation_requested()` can bail out promptly. Signal only —
    // the blocking wait itself is unchanged.
    ~Future() {
        if (f_.valid()) {
            if (token_) token_->cancelled.store(true, std::memory_order_relaxed);
        }
    }

    // The "await": implicit conversion blocks and returns the value.
    // Safe to call multiple times — caches the result after first get().
    operator T() { return get(); }

    T get() {
        if (!consumed_) {
            cached_ = f_.get();
            consumed_ = true;
        }
        return cached_;
    }

    bool ready() const {
        return consumed_ ||
               (f_.valid() &&
                f_.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    }

    // Set the cancel flag the worker task polls via
    // `xi::cancellation_requested()`. Cooperative — task must check.
    // Idempotent. Cancellation does NOT block; call get() to wait
    // for the task to actually return after observing the flag.
    void cancel() {
        if (token_) token_->cancelled.store(true, std::memory_order_relaxed);
    }
    bool cancelled() const {
        return token_ && token_->cancelled.load(std::memory_order_relaxed);
    }

private:
    std::future<T> f_;
    T cached_{};
    bool consumed_ = false;
    CancelTokenPtr token_;
};

// void specialization — no value, still a synchronization point.
template <>
class Future<void> {
public:
    Future() = default;
    Future(std::future<void>&& f, CancelTokenPtr tok)
        : f_(std::move(f)), token_(std::move(tok)) {}

    Future(Future&&) noexcept = default;
    Future& operator=(Future&&) noexcept = default;
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;

    // Same as Future<T>::~Future: signal cancel before the blocking wait.
    ~Future() {
        if (f_.valid()) {
            if (token_) token_->cancelled.store(true, std::memory_order_relaxed);
        }
    }

    void get() {
        if (!consumed_) { f_.get(); consumed_ = true; }
    }
    bool ready() const {
        return consumed_ ||
               (f_.valid() &&
                f_.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    }
    void cancel() {
        if (token_) token_->cancelled.store(true, std::memory_order_relaxed);
    }
    bool cancelled() const {
        return token_ && token_->cancelled.load(std::memory_order_relaxed);
    }

private:
    std::future<void> f_;
    bool consumed_ = false;
    CancelTokenPtr token_;
};

// xi::async(fn, args...) — spawn a task and return a Future<R>.
//
// Args are captured by value into the task closure (decay semantics) so the
// caller's locals can go out of scope safely.
template <class F, class... Args>
auto async(F&& f, Args&&... args)
    -> Future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
{
    using R = std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>;

    auto token = std::make_shared<CancelToken>();

    // C2: capture the image-pool owner active at spawn (the script during inspect,
    // or an instance if spawned inside process()) so async-created pool images are
    // attributed to it instead of anonymous (owner=0). Re-installed via OwnerScope
    // in the closure below. Zero cost when unwired (owner_get() returns 0).
    uint32_t parent_owner = detail::owner_get();

    // A4: capture the spawning thread's per-run context so the worker inherits the
    // run — xi::run_id() / xi::current_frame_path() / xi::result() resolve to the
    // correct run on the worker, and the host's off-thread detection is accurate.
    // Re-installed via RunCtxScope in the closure. Zero cost when unwired (null).
    const void* parent_run_ctx = detail::run_ctx_get();

    auto closure =
        [fn  = std::forward<F>(f),
         tup = std::make_tuple(std::forward<Args>(args)...),
         tok = token,
         parent_owner,
         parent_run_ctx]() mutable -> R {
            // Reserve fault-stack headroom FIRST (mirrors the dispatch lane +
            // one-shot worker paths) so the crash filter can still write a
            // minidump after a STACK_OVERFLOW on this worker; then install the
            // SEH translator so segfaults become seh_exception and propagate
            // through std::promise to the .get() / await site.
            xi::crash::reserve_fault_stack();
            xi::install_seh_translator();
            // Make this task's cancel token visible to
            // `xi::cancellation_requested()` for the duration of the user
            // callable. RAII restore on any exit path.
            struct Scope {
                CancelToken* prev;
                explicit Scope(CancelToken* t)
                    : prev(current_cancel_token_ref()) {
                    current_cancel_token_ref() = t;
                }
                ~Scope() { current_cancel_token_ref() = prev; }
            } scope(tok.get());
            // C2: re-install the parent's image-pool owner for the duration of the
            // user callable so any pool image it creates is attributed correctly.
            detail::OwnerScope owner_scope(parent_owner);
            // A4: re-install the parent's per-run context so xi::run_id() /
            // xi::current_frame_path() / xi::result() called from THIS worker
            // resolve to the run (the spawn-gap closure), and an off-thread
            // ambient current_trigger() read is still caught host-side.
            detail::RunCtxScope run_ctx_scope(parent_run_ctx);
            try {
                return std::apply(std::move(fn), std::move(tup));
            } catch (const xi::seh_exception& e) {
                // std::async(launch::async) runs on a pooled thread (MSVC threadpool)
                // that is REUSED for later tasks. A STACK_OVERFLOW here holed its guard
                // page; restore it (or hard-exit) before the fault is stored in the
                // promise and this thread returns to the pool. Then rethrow so the fault
                // still surfaces at the awaiting .get() on the spawning thread.
                xi::recover_seh_stack_or_die(e.code, "xi::async worker");
                throw;
            }
        };

    return Future<R>{std::async(std::launch::async, std::move(closure)), token};
}

// xi::await_all(f1, f2, ...) — wait for all futures, return a tuple of results.
// Void futures are accepted: they synchronise but contribute no tuple entry.
//   Future<int> a = ...;  Future<void> b = ...;  Future<Image> c = ...;
//   auto [i, img] = xi::await_all(a, b, c);  // b is awaited, not in tuple
template <class... Ts>
auto await_all(Future<Ts>&... fs) {
    auto one = [](auto& f) {
        using R = std::decay_t<decltype(f.get())>;
        if constexpr (std::is_void_v<R>) {
            f.get();
            return std::tuple<>{};
        } else {
            return std::make_tuple(f.get());
        }
    };
    return std::tuple_cat(one(fs)...);
}

} // namespace xi

//
// ASYNC_WRAP(name) — declare async_<name>(args...) that calls xi::async(name, args...).
//
// Put this after the sync declaration of an operator in the op library header:
//
//   Image gaussian(const Image& in, double sigma);
//   ASYNC_WRAP(gaussian)
//
// ...gives users async_gaussian(gray, 3.0) returning Future<Image>.
//
#define ASYNC_WRAP(fn)                                                         \
    template <class... XiArgs>                                                 \
    inline auto async_##fn(XiArgs&&... xi_args) {                              \
        return ::xi::async(fn, std::forward<XiArgs>(xi_args)...);              \
    }
