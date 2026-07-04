// sem_queue.cpp — a "custom queue" implemented ENTIRELY in a plugin: N-way
// admission via a counting semaphore, with the ORDERED output ("envelope")
// coming for free from the core's per-lane emit gate. NO CORE CHANGES — this
// composes existing primitives. The concrete proof of docs/new_gen/24's thesis:
//
//     the PLUGIN owns ADMISSION (custody); the CORE owns the ENVELOPE (mechanism).
//
// ONE DLL, TWO ROLES (both share the process-global semaphore via file-scope
// statics — a single in_process module load ⇒ statics shared across instances):
//
//   * role "source" (sem_src): owns a DEEP internal backlog (std::deque). A
//     dedicated producer thread loops:  g_sem.acquire() (blocks when N permits
//     are in flight) → pop the next item → emit() it as a trigger into the
//     dispatch lane. At most N items are ever in flight — the SEMAPHORE, not the
//     core lane, is the sole admission control ("depth 0" custody: the core lane
//     never accumulates a backlog; the plugin's deque holds it all).
//
//   * ack (sem_ack): the completion signal. Its xi.pack@1 door releases ONE
//     permit ("一個 thread 算完 release"). The inspect script drives it INLINE as
//     its last step (xi::use("sem_ack").process(marker)) → the release fires on
//     the worker thread at compute-completion. sem_ack is NOT a declared sink, so
//     process() runs synchronously here rather than being staged.
//
// INSTRUMENTATION: g_inflight = acquired-not-released (the in-flight custody
// count), g_max_inflight = its running max. Both are process-global; either
// instance's get_def() surfaces them so the driver can assert max in-flight == N
// (reaches N under load AND never exceeds N).
//
// SEMAPHORE PRIMITIVE: a counting semaphore built from mutex + condition_variable
// + int counter, NOT std::counting_semaphore. Both compile under this MSVC C++20
// toolchain; the cv version is chosen deliberately because (a) it seeds N permits
// at RUNTIME (from config) with a clean per-run reset, and (b) its acquire() is
// INTERRUPTIBLE — a stop/teardown wakes a parked producer (abort()) so the join
// returns bounded, whereas std::counting_semaphore::acquire() is uninterruptible
// (teardown would need a try_acquire_for polling loop or a wake-by-release hack).
// Same stop-wake discipline as the depth-0 rendezvous (doc 24 §3).
//
// TEARDOWN NOTE: on stop the semaphore is simply ABANDONED — a frame that was
// admitted but whose inspect is released mid-flight by cmd:stop never reaches the
// ack door, so its permit leaks. That is benign here (the producer is woken by
// abort(), not by a permit). A production version would also release on
// drop/stop (RAII permit tied to the frame's lifetime).
//
// isolation must be "in_process".
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr uint64_t SEMQ_TID_HI = 0x73656d7175650000ull;  // "semqu"

// A counting semaphore (mutex + cv + counter). Process-global; shared by every
// instance of this DLL. Interruptible: acquire() also returns on abort()/!running
// so a parked producer unblocks at teardown.
//
// In-flight instrumentation lives HERE, under the same lock as the permit count,
// so it is EXACT: in_flight == total - permits (permits currently taken), and the
// peak is recorded at acquire time under the lock. Tracking it with separate
// atomics OUTSIDE the lock races — a decrement (worker, post-release) can lag an
// increment (source, post-acquire) and momentarily read N+1 even though the
// semaphore never lets more than N permits be taken.
class CountingSem {
public:
    void seed(int n) {
        std::lock_guard<std::mutex> lk(mu_);
        total_        = n;
        permits_      = n;
        max_inflight_ = 0;
        abort_        = false;
    }
    // Block until a permit is free. Returns true with a permit taken; false if
    // aborted (teardown) or the producer stopped — no permit taken then.
    bool acquire(const std::atomic<bool>& running) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [&] { return permits_ > 0 || abort_ || !running.load(); });
        if (abort_ || !running.load()) return false;
        --permits_;
        const int inflight = total_ - permits_;          // exact, under the lock
        if (inflight > max_inflight_) max_inflight_ = inflight;
        return true;
    }
    void release() {
        { std::lock_guard<std::mutex> lk(mu_); if (permits_ < total_) ++permits_; }
        cv_.notify_one();
    }
    // Wake every parked acquire() (teardown). Permits are abandoned, not drained.
    void abort() {
        { std::lock_guard<std::mutex> lk(mu_); abort_ = true; }
        cv_.notify_all();
    }
    int permits()      { std::lock_guard<std::mutex> lk(mu_); return permits_; }
    int inflight()     { std::lock_guard<std::mutex> lk(mu_); return total_ - permits_; }
    int max_inflight() { std::lock_guard<std::mutex> lk(mu_); return max_inflight_; }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    int                     total_        = 0;   // N
    int                     permits_      = 0;   // free permits
    int                     max_inflight_ = 0;   // peak (total_ - permits_), at acquire
    bool                    abort_        = false;
};

CountingSem      g_sem;               // the N-permit admission gate (shared)
std::atomic<int> g_permits_cfg{4};    // N — set from config; seeded on source start
}  // namespace

class SemQueue : public xi::Plugin {
public:
    SemQueue(const xi_host_api* host, const std::string& name) : xi::Plugin(host, name) {}
    ~SemQueue() override { stop_(); }

    std::string get_def() const override {
        return xi::Json::object()
            .set("role",         role_)
            .set("running",      running_.load())
            .set("permits",      g_permits_cfg.load())              // N
            .set("permits_now",  g_sem.permits())                  // free permits right now
            .set("backlog",      backlog_.load())                   // plugin-owned queue depth
            .set("emitted",      (int)(emitted_.load() & 0x7fffffff))
            .set("released",     (int)(released_.load() & 0x7fffffff))
            .set("inflight",     g_sem.inflight())                  // current custody (exact)
            .set("max_inflight", g_sem.max_inflight())              // peak custody (== N under load)
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        role_ = p["role"].as_string(role_);
        int b = p["backlog"].as_int(backlog_.load()); if (b < 1) b = 1;
        backlog_.store(b);
        int n = p["permits"].as_int(g_permits_cfg.load()); if (n < 1) n = 1;
        g_permits_cfg.store(n);
        return true;
    }
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto command = p["command"].as_string();
        if (command == "start")     start_();
        else if (command == "stop") stop_();
        // "stat" (or any other) falls through to get_def() below — the driver
        // reads the shared instrumentation that way.
        return get_def();
    }

    // THE ACK DOOR — release one permit on completion ("一個 thread 算完 release").
    // Driven INLINE by xi::use("sem_ack").process(marker) as the inspect's last
    // step, so it runs on the worker thread exactly when that frame's compute
    // finishes. One custody slot freed → g_inflight--.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        (void)in;
        g_sem.release();                                   // one custody slot freed
        const uint64_t r = released_.fetch_add(1) + 1;
        out.i64("released", (int64_t)(r & 0x7fffffff));
        out.i64("inflight", g_sem.inflight());
    }

private:
    std::string           role_ = "source";
    std::atomic<bool>     running_{false};
    std::atomic<int>      backlog_{200};      // THE plugin-owned queue depth
    std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> emitted_{0};
    std::atomic<uint64_t> released_{0};
    std::deque<uint64_t>  pending_;           // THE custom queue (plugin-owned backlog)
    std::mutex            pending_mu_;
    std::thread           thread_;

    void start_() {
        if (running_.load()) return;
        // Seed the SHARED semaphore with N permits + reset the SHARED instrumentation
        // (clean per-run counters — this is why a runtime-seedable cv semaphore beats
        // a static std::counting_semaphore here).
        g_sem.seed(g_permits_cfg.load());   // seeds permits + resets peak instrumentation
        seq_.store(0);
        emitted_.store(0);
        released_.store(0);
        {
            std::lock_guard<std::mutex> lk(pending_mu_);
            pending_.clear();
            const int n = backlog_.load();
            for (int i = 0; i < n; ++i) pending_.push_back((uint64_t)i);
        }
        running_ = true;
        thread_  = std::thread([this] { run_loop_(); });
    }

    void stop_() {
        running_ = false;
        g_sem.abort();  // wake a producer parked in acquire() → bounded teardown
        if (thread_.joinable()) thread_.join();
    }

    void emit_one_() {
        const int W = 16, H = 16;
        const uint64_t seq = seq_.fetch_add(1);
        std::vector<uint8_t> px((size_t)W * H, 128);
        std::memcpy(px.data(), &seq, sizeof(seq));  // seq in first 8 bytes
        xi::PackOut out = new_pack();
        out.image("img", W, H, 1, px.data());
        xi_trigger_id tid;
        tid.hi = SEMQ_TID_HI;
        tid.lo = seq + 1;
        emit(std::move(out), tid);  // hand into the dispatch lane
        emitted_.fetch_add(1);
    }

    void run_loop_() {
        while (running_.load()) {
            // ADMISSION: block until a permit is free (≤ N in flight). The
            // SEMAPHORE is the sole admission control; the core lane never
            // accumulates a backlog.
            // A permit is held: this item is now IN FLIGHT (custody). The peak is
            // recorded inside acquire(), under the semaphore lock (exact).
            if (!g_sem.acquire(running_)) break;  // aborted → teardown

            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                if (pending_.empty()) {
                    g_sem.release();   // backlog drained: hand the permit back, finish
                    break;
                }
                pending_.pop_front();
            }
            emit_one_();  // worker computes, then the ack door releases the permit
        }
        running_ = false;
    }
};

XI_PLUGIN_IMPL(SemQueue)
XI_PLUGIN_PACK_DOOR(SemQueue)
