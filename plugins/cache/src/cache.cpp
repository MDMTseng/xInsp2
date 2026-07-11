// buffer_replay.cpp — replay as plugin composition.
//
// The host used to own record/replay; in the ABI-v6 dispatch model that's a
// plugin's job. buffer_replay is the reference for it: process(in) captures the
// incoming record into a bounded ring; an exchange command re-emits a buffered
// record via emit() so the script re-runs on it. That is the
// HDevelop-style hot-param loop — buffer a frame, tune a Param, "replay_last" to
// re-inspect the SAME frame with the new value, no camera re-grab.
//
//   in the inspect script:
//     auto t = xi::current_trigger();
//     xi::use("buffer").process(t.pack());   // capture the current sealed pack
//     ... inspect with a tunable Param ...
//   then, from the UI / a test (cmd as an OBJECT, not a string):
//     exchange_instance("buffer", {"command":"replay_last"})  // re-inspect last frame
//
// replay_last / replay_all / replay emit back-to-back (as fast as dispatch will
// take them). replay_timed re-emits paced by the ORIGINAL inter-capture gaps
// (optionally time-scaled) on a background thread, reproducing the arrival
// cadence — the "timed" replay mode. It does NOT reproduce byte-identical
// ordering (that is on-disk deterministic replay, a separate feature); it
// reproduces load/timing shape.
//
// polaris2 wave-2 — BILINGUAL (docs/new_gen/07 + 10 gate P1). buffer_replay now
// speaks BOTH currencies through ONE ring: a Record entry is a deep-copied
// Record (below), a Pack entry is a RETAINED reference to a sealed host Pack
// (xi.pack@1). Retaining a sealed pack keeps it — and its pool image handles —
// alive BEYOND the frame that produced it; replay re-emits the SAME sealed pack
// handle with a fresh trigger id (zero pixel copy — proven by a stable pool
// live-count across replay). Eviction / clear / teardown release every retained
// pack, exactly as `release()` on the ring's owning ref. The replay commands are
// ring-agnostic: they work on whichever entry kind lives at the target slot, and
// replay_timed paces one loop over the variant entry (records + packs interleave
// and replay in capture order). See README § "Pack retention".

#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_thread.hpp>   // xi::spawn_worker — SEH-safe replay thread
#include <xi/xi_contract.hpp>   // fail-loud required command payloads + schema skew

#include "cache_keys.h"         // guard 1: command/config/output key names, once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>   // replay_timed owner-ledger handoff (see start_timed_)
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Guard 1: the plugin's own readers compile from the SAME constants the typed
// view (cache_io.h) builds from, so a key rename can't drift.
namespace keys = xi::cache::keys;

class BufferReplay : public xi::Plugin {
public:
    using xi::Plugin::Plugin;
    // Teardown honesty: stop the replay worker (it releases its own snapshot
    // refs on exit), THEN release every pack the ring still owns. Releasing here
    // is the plugin's job — the host has no pack owner-sweep analogue of the
    // ImagePool leak-sweep (see README § "Pack retention"), so a missed release
    // would leak a sealed pack until static teardown. We don't miss it.
    ~BufferReplay() override {
        stop_replay_();
        std::vector<xi_pack_handle> reclaim;
        { std::lock_guard<std::mutex> lk(mu_); drain_ring_locked_(reclaim); }
        release_packs_(reclaim);
    }

    // v12: the xi.pack@1 capture door is THE data plane. RETAIN the incoming
    // sealed pack into the ring (bounded capacity/eviction — evict = release), and
    // ack with the buffered count {buffered:N}. The retained ref keeps the sealed
    // pack + its pool handles alive past this frame; replay re-emits it (zero
    // copy). A host with no pack plane never calls this door.
    void process(xi::PackIn& in, xi::PackOut& out) override {
        const xi_pack_v1* fi = pack_iface();
        std::vector<xi_pack_handle> reclaim;
        int size;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (fi && in.valid()) {
                fi->retain(in.handle());                       // the ring's owning ref
                ring_.push_back(Entry::from_pack(in.handle(), mono_us_()));
                trim_locked_(reclaim);
            }
            size = (int)ring_.size();
        }
        release_packs_(reclaim);                                // evicted entries
        out.i64(keys::kBuffered, (int64_t)size);                // same ack shape as the Record path
    }

    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        auto c = p[keys::kCommand].as_string();

        // Guard 2: commands whose payload has NO sensible default fail loud
        // rather than silently doing nothing. "replay" needs a target index;
        // "set_capacity" needs a size. (replay_last/replay_all/replay_timed all
        // have well-defined defaults, so they stay tolerant.) Unknown commands
        // still fall through to get_def() below — a driver "status" idiom.
        if (c == keys::kReplay) {
            auto idx = p[keys::kIndex];
            if (!idx.valid())     return xi::contract::fault_json(xi::contract::kMissingInput, keys::kIndex, "int");
            if (!idx.is_number()) return xi::contract::fault_json(xi::contract::kWrongType,   keys::kIndex, "int");
        }
        if (c == keys::kSetCapacity) {
            auto v = p[keys::kValue];
            if (!v.valid())     return xi::contract::fault_json(xi::contract::kMissingInput, keys::kValue, "int");
            if (!v.is_number()) return xi::contract::fault_json(xi::contract::kWrongType,   keys::kValue, "int");
        }

        if (c == keys::kReplayTimed) {
            // Paced replay of the buffered entries on a background thread, so the
            // exchange returns quickly. `speed` scales the recorded gaps
            // (>1 faster, default 1.0). `n` optionally limits to the last n. The
            // snapshot RETAINS each pack ref (the worker outlives the lock and may
            // race a clear/evict) — but that ref is only a BRIDGE: start_timed_
            // hands ownership to the worker via its own owner-neutral refs and
            // releases the bridge refs back on THIS thread, so the owner-tagged
            // retain/release stay symmetric (see start_timed_ for why).
            double speed = p[keys::kSpeed].as_double(1.0); if (speed <= 0.0) speed = 1.0;
            int    k     = p[keys::kN].as_int(-1);
            std::vector<Entry> snap;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const int n = (int)ring_.size();
                int start = (k > 0 && k < n) ? n - k : 0;
                for (int i = start; i < n; ++i) snap.push_back(snapshot_locked_(ring_[(size_t)i]));
            }
            start_timed_(std::move(snap), speed);
            return get_def();
        }

        // Snapshot the entries to replay UNDER the lock (retaining pack refs), then
        // emit OUTSIDE it (emit dispatches; the re-run may feed process() back in).
        // Pack handles reclaimed by clear/set_capacity are collected and released
        // after unlocking, same discipline.
        std::vector<Entry> to_emit;
        std::vector<xi_pack_handle> reclaim;
        {
            std::lock_guard<std::mutex> lk(mu_);
            const int n = (int)ring_.size();
            if (c == keys::kReplayLast) {
                int k = p[keys::kN].as_int(1); if (k < 1) k = 1; if (k > n) k = n;
                for (int i = n - k; i < n; ++i) to_emit.push_back(snapshot_locked_(ring_[(size_t)i]));
            } else if (c == keys::kReplayAll) {
                for (auto& e : ring_) to_emit.push_back(snapshot_locked_(e));
            } else if (c == keys::kReplay) {
                int i = p[keys::kIndex].as_int(-1);
                if (i >= 0 && i < n) to_emit.push_back(snapshot_locked_(ring_[(size_t)i]));
            } else if (c == keys::kStopReplay) {
                // handled below (must not hold the lock while joining)
            } else if (c == keys::kClear) {
                drain_ring_locked_(reclaim);   // release every retained pack + empty
            } else if (c == keys::kSetCapacity) {
                int v = p[keys::kValue].as_int(capacity_); if (v < 1) v = 1;
                capacity_ = v;
                trim_locked_(reclaim);
            }
        }
        if (c == keys::kStopReplay || c == keys::kClear) stop_replay_();
        emit_snapshot_(to_emit);    // emits each entry then releases its pack refs
        release_packs_(reclaim);
        return get_def();
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        return xi::Json::object()
            .set(keys::kCapacity, capacity_)
            .set(keys::kCount, (int)ring_.size())
            .set(keys::kPacks, packs_locked_())
            .set(keys::kReplaying, replaying_.load())
            .dump();
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        // Guard 3: reject a config built against an incompatible header schema
        // (a matching or absent stamp proceeds — legacy persisted defs have none).
        auto sv = p[xi::contract::kSchemaKey];
        if (sv.is_number() && sv.as_int() != xi::cache::kSchemaVersion) {
            log_error("cache: config schema mismatch: built for v" +
                      std::to_string(sv.as_int()) + ", this plugin serves v" +
                      std::to_string(xi::cache::kSchemaVersion));
            return false;
        }
        int v = p[keys::kCapacity].as_int(capacity_); if (v < 1) v = 1;
        std::vector<xi_pack_handle> reclaim;
        {
            std::lock_guard<std::mutex> lk(mu_);
            capacity_ = v;
            trim_locked_(reclaim);   // a shrink may evict retained packs
        }
        release_packs_(reclaim);
        return true;
    }

private:
    // A ring entry OWNS one host Pack ref (retained at capture, released on
    // evict/clear/teardown). t_us is the monotonic capture time carried for
    // replay_timed pacing. (v12: the Record variant is gone — packs are the sole
    // data plane, so every entry is a pack.)
    struct Entry {
        xi_pack_handle pack = XI_PACK_NULL;
        int64_t        t_us = 0;
        bool is_pack() const { return pack != XI_PACK_NULL; }
        static Entry from_pack(xi_pack_handle h, int64_t t) { Entry e; e.pack = h; e.t_us = t; return e; }
    };

    static int64_t mono_us_() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // --- ring helpers (caller holds mu_) ------------------------------------

    // Copy an entry for a replay snapshot, retaining an INDEPENDENT pack ref so
    // the snapshot survives a concurrent clear/evict of the ring's own ref.
    //
    // OWNER-LEDGER CONTRACT: fi->retain here runs on the exchange thread, inside
    // the adapter's ImagePool::OwnerGuard, so the ref is TAGGED to this cache
    // instance's ledger bucket (retain_as(pack, cache_owner)). It must therefore
    // be released on a thread with the SAME owner context, or the bucket is
    // never decremented (release_as(pack, 0) is a documented ledger no-op) and
    // teardown's release_all_for over-sweeps — a data-plane UAF. The synchronous
    // paths (emit_snapshot_ for replay/replay_last/replay_all, and the dtor)
    // release on this same guarded thread, so they are symmetric as-is. The
    // replay_timed worker CANNOT release these refs (its current_owner() is 0);
    // start_timed_ instead swaps them for the worker's own owner-neutral refs
    // and releases these bridge refs back on the exchange thread.
    Entry snapshot_locked_(const Entry& e) {
        Entry s = e;
        if (s.is_pack()) { if (const xi_pack_v1* fi = pack_iface()) fi->retain(s.pack); }
        return s;
    }

    // Evict from the front while over capacity, collecting evicted pack refs to
    // release after the lock (a pack entry's owning ref is dropped on eviction).
    void trim_locked_(std::vector<xi_pack_handle>& reclaim) {
        while ((int)ring_.size() > capacity_) {
            if (ring_.front().is_pack()) reclaim.push_back(ring_.front().pack);
            ring_.pop_front();
        }
    }

    // Empty the ring, collecting every retained pack ref to release after unlock.
    void drain_ring_locked_(std::vector<xi_pack_handle>& reclaim) {
        for (auto& e : ring_) if (e.is_pack()) reclaim.push_back(e.pack);
        ring_.clear();
    }

    int packs_locked_() const {
        int n = 0; for (auto& e : ring_) if (e.is_pack()) ++n; return n;
    }

    // --- emit / release (no lock held) --------------------------------------

    void release_packs_(const std::vector<xi_pack_handle>& hs) {
        if (hs.empty()) return;
        const xi_pack_v1* fi = pack_iface();
        if (fi) for (auto h : hs) fi->release(h);
    }

    // Re-emit ONE buffered entry with a fresh trigger id. It re-emits the SAME
    // sealed pack handle via the host emit door (emit_pack takes its own event
    // ref — zero copy, the snapshot/ring refs are untouched).
    void emit_entry_(const Entry& e) {
        if (e.is_pack()) {
            if (const xi_pack_v1* fi = pack_iface())
                fi->emit_pack(name().c_str(), XI_TRIGGER_NULL, e.pack, 0);  // fresh id, ts = now
        }
    }

    // Emit each snapshot entry, then release the snapshot's retained pack refs.
    void emit_snapshot_(std::vector<Entry>& snap) {
        for (auto& e : snap) emit_entry_(e);
        const xi_pack_v1* fi = pack_iface();
        if (fi) for (auto& e : snap) if (e.is_pack()) fi->release(e.pack);
        snap.clear();
    }

    // Stop any in-flight timed replay and join its thread. Safe to call from the
    // WS/exchange thread and the destructor; never called from the replay thread.
    void stop_replay_() {
        replay_stop_.store(true);
        if (replay_thread_.joinable()) replay_thread_.join();
        replay_stop_.store(false);
        replaying_.store(false);
    }

    void start_timed_(std::vector<Entry> snap, double speed) {
        stop_replay_();                       // cancel a prior replay first
        if (snap.empty()) return;
        replaying_.store(true);

        // OWNER-LEDGER SYMMETRY (P0 UAF fix). The snapshot refs were retained on
        // the EXCHANGE thread inside the adapter's OwnerGuard, so they are tagged
        // to this instance's PackRegistry ledger bucket. The worker thread has NO
        // owner context (current_owner() == 0), so if it released those refs the
        // ledger decrement would be a no-op — the bucket would inflate by +1 per
        // timed-replayed entry, forever, and teardown's release_all_for(cache)
        // would then over-reclaim rc (including live untagged dispatch-event
        // refs) and free a pack under a live reader. The xi_pack_v1 vtable
        // exposes no untagged retain and the SDK no way to set owner context on
        // a plugin thread, so we make BOTH pairs symmetric via a handoff:
        //   1. the tagged snapshot refs act only as a BRIDGE keeping the packs
        //      alive across the thread start (a clear/evict may race it);
        //   2. the worker FIRST takes its own refs — fi->retain on the ownerless
        //      worker thread is retain_as(pack, 0): rc-only, ledger untouched,
        //      exactly a framework-transient ref — then signals;
        //   3. we then release the bridge refs HERE, on the same guarded
        //      exchange thread that took them, decrementing the same bucket.
        // Worker refs: retained owner-0, released owner-0 (ledger no-op both
        // ends). Bridge refs: retained tagged, released tagged, same thread.
        std::vector<xi_pack_handle> bridge;
        for (const auto& e : snap) if (e.is_pack()) bridge.push_back(e.pack);
        auto owned = std::make_shared<std::promise<void>>();
        std::future<void> owned_f = owned->get_future();

        // Blessed worker: xi::spawn_worker installs the per-thread SEH translator
        // + top-level catch so a fault mid-replay is contained to this thread
        // rather than taking down the whole backend (a raw std::thread would not).
        // ONE loop over the variant entry — records + packs interleave and replay
        // in capture order; the worker's own (owner-neutral) pack refs are
        // released on exit (every path, including an early stop) so nothing leaks.
        replay_thread_ = xi::spawn_worker(name() + "-replay", [this, snap = std::move(snap), speed, owned]() mutable {
            // Take this thread's OWN refs before anything else: current_owner()
            // is 0 here, so these are untagged (rc-only) — symmetric with the
            // untagged releases below. Signal the exchange thread afterwards so
            // it can drop its tagged bridge refs; the bridge keeps every pack
            // alive until this point, so no handle can die under us.
            {
                const xi_pack_v1* fi = pack_iface();
                if (fi) for (auto& e : snap) if (e.is_pack()) fi->retain(e.pack);
                owned->set_value();
            }
            for (size_t i = 0; i < snap.size(); ++i) {
                if (replay_stop_.load()) break;
                if (i > 0) {
                    int64_t dt = snap[i].t_us - snap[i - 1].t_us;
                    if (dt < 0) dt = 0;
                    int64_t wait_us = (int64_t)((double)dt / speed);
                    // Interruptible sleep so stop_replay_/clear cancels promptly.
                    for (int64_t s = 0; s < wait_us && !replay_stop_.load(); ) {
                        int64_t chunk = std::min<int64_t>(wait_us - s, 20000);
                        std::this_thread::sleep_for(std::chrono::microseconds(chunk));
                        s += chunk;
                    }
                    if (replay_stop_.load()) break;
                }
                emit_entry_(snap[i]);
            }
            // Release the refs THIS thread retained above: owner 0 on retain,
            // owner 0 on release — the ledger is untouched on both ends, only
            // rc moves. (Never release an exchange-thread-tagged ref here.)
            const xi_pack_v1* fi = pack_iface();
            if (fi) for (auto& e : snap) if (e.is_pack()) fi->release(e.pack);
            replaying_.store(false);
        });

        // Handoff complete? (A broken promise — worker faulted before signaling
        // — also readies the future; the SEH containment in spawn_worker owns
        // that path.) Then drop the tagged bridge refs on THIS thread, inside
        // the same OwnerGuard that tagged them in snapshot_locked_: retain and
        // release hit the same ledger bucket, symmetric by construction.
        owned_f.wait();
        release_packs_(bridge);
    }

    mutable std::mutex     mu_;
    std::deque<Entry>      ring_;
    int                    capacity_ = 16;

    std::thread            replay_thread_;
    std::atomic<bool>      replay_stop_{false};
    std::atomic<bool>      replaying_{false};
};

XI_PLUGIN_IMPL(BufferReplay)
// polaris2 wave-2: publish the xi.pack@1 pack-in/pack-out door — the capture
// side of the bilingual ring (the host probes xi_plugin_get_interface("xi.pack",
// 1) to learn cache speaks packs).
XI_PLUGIN_PACK_DOOR(BufferReplay)
