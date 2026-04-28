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
#include <future>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace xi {

// Cancellation token. Shared between a Future<T> and the worker
// task it spawned. Caller-side `cancel()` flips the bit; worker-
// side `is_cancelled()` polls. Cooperation is required — a long-
// running op that never polls won't notice.
//
// Ops in the `xi::ops` library should call
// `xi::cancellation_requested()` (below) at loop heads / on chunk
// boundaries to make themselves abort-friendly. Anything that doesn't
// is documented as "uncancellable" and runs to completion regardless
// of token state.
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

    auto closure =
        [fn  = std::forward<F>(f),
         tup = std::make_tuple(std::forward<Args>(args)...),
         tok = token]() mutable -> R {
            // Install SEH translator on the worker thread so segfaults
            // become seh_exception and propagate through std::promise
            // to the .get() / await site.
            xi::install_seh_translator();
            // Make this token visible to `xi::cancellation_requested()`
            // for the duration of the user callable. RAII restore on
            // any exit path.
            struct Scope {
                CancelToken* prev;
                explicit Scope(CancelToken* t) : prev(current_cancel_token_ref()) {
                    current_cancel_token_ref() = t;
                }
                ~Scope() { current_cancel_token_ref() = prev; }
            } scope(tok.get());
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
