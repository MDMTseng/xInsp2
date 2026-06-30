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

// RAII: install `id` as this thread's image-pool owner for the scope, restore the
// previous owner on exit. Symmetric with the cancel-token / inspect-ticket Scope
// — used by xi::async and xi::parallel_for to attribute worker-created images.
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

// Watchdog cooperative-cancel, scoped by inspect EPOCH.
//
// The host's watchdog cancels the inspect(s) that were ALREADY in flight when
// it tripped — NOT a fresh inspect that the dispatch pool starts during the
// 1000ms grace. The old design held a single global bool for the whole grace,
// so every heavy frame dispatched in that ~1s window observed the stale request
// and aborted (≈30 spurious cancellations per slow frame at 30fps). We scope
// the cancel with a monotonic ticket instead:
//
//   - cancel_ticket_counter(): strictly-increasing source of inspect tickets.
//   - begin_inspect(): each inspect draws a fresh ticket at start (host calls
//     the `xi_script_inspect_begin` thunk on the dispatch thread before
//     s.inspect()), installed thread-local so the inspect — and any xi::async
//     sub-task it spawns — share one cancel scope.
//   - arm_cancel(): the watchdog snapshots the counter's high-water as the
//     cutoff and marks cancel active. Every inspect whose start-ticket is
//     BELOW the cutoff (i.e. was in flight at trip time) sees the cancel; an
//     inspect that draws its ticket after the snapshot (>= cutoff) does NOT.
//   - clear_cancel(): the watchdog clears `active` once the targeted inspects
//     have returned. (A genuinely-stuck inspect still overruns next watchdog
//     tick → a fresh arm_cancel with a higher cutoff re-targets it, and the
//     hard-trip escalation still exits the process — see service_main.cpp.)
//
// These live in the calling TU (script DLL or backend): the backend drives the
// script DLL's copies via the exported thunks in xi_script_support.hpp.
//
// `cancellation_requested()` returns true when the per-inspect cancel applies
// OR the per-task xi::async token is set. It is on the hot inspect poll path:
// the cost is one relaxed bool load (early-out when no cancel is armed) plus,
// only while a cancel IS armed, two relaxed uint64 loads — no locks.
inline std::atomic<uint64_t>& cancel_ticket_counter() {
    static std::atomic<uint64_t> c{1};   // 0 reserved: "no ticket / legacy"
    return c;
}
inline std::atomic<uint64_t>& cancel_cutoff() {
    static std::atomic<uint64_t> c{0};
    return c;
}
inline std::atomic<bool>& cancel_active() {
    static std::atomic<bool> a{false};
    return a;
}

// This thread's current inspect ticket (0 = not inside a ticketed inspect).
// xi::async copies it into the worker so sub-tasks share the parent's scope.
inline uint64_t& current_inspect_ticket_ref() {
    static thread_local uint64_t t = 0;
    return t;
}

// Draw a fresh, strictly-increasing ticket for an inspect about to run and
// install it on this thread. Returns the ticket. Called once per inspect start.
inline uint64_t begin_inspect() {
    uint64_t t = cancel_ticket_counter().fetch_add(1, std::memory_order_relaxed);
    current_inspect_ticket_ref() = t;
    return t;
}

// Watchdog trip: target every inspect in flight RIGHT NOW (start-ticket below
// the current high-water) — but never one that starts afterwards. Idempotent.
inline void arm_cancel() {
    cancel_cutoff().store(cancel_ticket_counter().load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    cancel_active().store(true, std::memory_order_relaxed);
}
inline void clear_cancel() {
    cancel_active().store(false, std::memory_order_relaxed);
}

inline bool cancellation_requested() {
    if (cancel_active().load(std::memory_order_relaxed)) {
        uint64_t my = current_inspect_ticket_ref();
        // my == 0 ⇒ no ticket was drawn (legacy script lacking the
        // inspect-begin hook, or code running outside any inspect). Treat it as
        // "in flight" so the watchdog's cooperative cancel still reaches such
        // code — preserves the pre-epoch global-cancel behaviour for them.
        if (my == 0 || my < cancel_cutoff().load(std::memory_order_relaxed))
            return true;
    }
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

    // Capture the spawning inspect's ticket so the worker shares its cancel
    // scope (the worker runs on a fresh thread whose thread_local ticket would
    // otherwise be 0 = "legacy/in-flight" and wrongly observe an unrelated
    // trip's cancel).
    uint64_t parent_ticket = current_inspect_ticket_ref();

    // C2: capture the image-pool owner active at spawn (the script during inspect,
    // or an instance if spawned inside process()) so async-created pool images are
    // attributed to it instead of anonymous (owner=0). Re-installed via OwnerScope
    // in the closure below. Zero cost when unwired (owner_get() returns 0).
    uint32_t parent_owner = detail::owner_get();

    auto closure =
        [fn  = std::forward<F>(f),
         tup = std::make_tuple(std::forward<Args>(args)...),
         tok = token,
         parent_ticket,
         parent_owner]() mutable -> R {
            // Install SEH translator on the worker thread so segfaults
            // become seh_exception and propagate through std::promise
            // to the .get() / await site.
            xi::install_seh_translator();
            // Make this token + the parent inspect's ticket visible to
            // `xi::cancellation_requested()` for the duration of the user
            // callable. RAII restore on any exit path.
            struct Scope {
                CancelToken* prev;
                uint64_t     prev_ticket;
                Scope(CancelToken* t, uint64_t ticket)
                    : prev(current_cancel_token_ref()),
                      prev_ticket(current_inspect_ticket_ref()) {
                    current_cancel_token_ref()   = t;
                    current_inspect_ticket_ref() = ticket;
                }
                ~Scope() {
                    current_cancel_token_ref()   = prev;
                    current_inspect_ticket_ref() = prev_ticket;
                }
            } scope(tok.get(), parent_ticket);
            // C2: re-install the parent's image-pool owner for the duration of the
            // user callable so any pool image it creates is attributed correctly.
            detail::OwnerScope owner_scope(parent_owner);
            return std::apply(std::move(fn), std::move(tup));
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
