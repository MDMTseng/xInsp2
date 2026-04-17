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

#include <future>
#include <tuple>
#include <type_traits>
#include <utility>

namespace xi {

template <class T>
class Future {
public:
    Future() = default;
    explicit Future(std::future<T>&& f) : f_(std::move(f)) {}

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

private:
    std::future<T> f_;
    T cached_{};
    bool consumed_ = false;
};

// void specialization — no value, still a synchronization point.
template <>
class Future<void> {
public:
    Future() = default;
    explicit Future(std::future<void>&& f) : f_(std::move(f)) {}

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

private:
    std::future<void> f_;
    bool consumed_ = false;
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

    auto closure =
        [fn  = std::forward<F>(f),
         tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
            return std::apply(std::move(fn), std::move(tup));
        };

    return Future<R>{std::async(std::launch::async, std::move(closure))};
}

// xi::await_all(f1, f2, ...) — wait for all futures, return a tuple of results.
// Void futures are accepted but contribute nothing to the tuple.
template <class... Ts>
auto await_all(Future<Ts>&... fs) {
    return std::make_tuple(fs.get()...);
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
