//
// lut_owner.cpp — demo.lut, the RESOURCE-HANDLE PATTERN demo lib plugin
// (docs/new_gen/14, appendix "Resource-handle convention").
//
// A TYPE-OWNER lib plugin: heavy custom objects (here: an immutable sorted
// i64 -> i64 lookup table — the minimal honest stand-in for any
// build-once-query-many structure) never ride packs. This DLL constructs and
// destructs them; packs carry only the HANDLE ENTRY, a nested canonical-mp
// map:
//
//     { "type": "demo.lut", "id": <i64 slot>, "gen": <i64 generation>, "$v": 1 }
//
// The five convention rules, as implemented here:
//   1. ALL alloc/free inside this DLL — the ring below owns every Lut object;
//      nothing crosses the ABI but the handle entry and query answers.
//   2. Objects are IMMUTABLE after construction (seal semantics): a Lut is a
//      const sorted vector behind shared_ptr<const Lut>; mutation = build new.
//   3. Lifetime is a RING/GENERATION LEASE (pre-v12): N slots, LRU-recycled
//      under pressure; every recycle bumps the slot generation from a
//      DLL-lifetime monotonic counter (never reused, even across instance
//      reinit — a stale handle can never alias a fresh object). A stale
//      resolve answers a sealed $fault "stale_handle" pack; owner sweep on
//      crash is the instance lifecycle itself (the ring dies with the
//      instance; capability registrations are owner-swept like imgcodec).
//   4. Persistence: handle entries are RUNTIME-ONLY. The type owner registers
//      the DUMP capability (demo.lut.dump) as the materializer — a persist
//      sink either materializes the record (byte-deterministic canonical bin)
//      or drops the entry; it never stores the handle.
//   5. A resolve with the wrong "type" answers $fault "wrong_type" — a handle
//      is only meaningful to its owning namespace.
//
// Capabilities (name-only registry; versioning rides IN the request pack):
//
//   "demo.lut.build" — Pack in:  mp "keys" (canonical array of i64),
//                                mp "values" (array of i64, same length),
//                                optional "$v" (supported: 1) / "$probe".
//                      Pack out: mp "handle" (the entry above), i64 "built"
//                                (1 = constructed now, 0 = content-dedup hit),
//                                i64 "builds" (lifetime build counter — the
//                                zero-rebuild proof), i64 "size".
//                      DEDUP: content-keyed (FNV-1a over the canonical key/
//                      value bytes) — the same sealed content builds ONCE and
//                      every consumer receives the same handle.
//
//   "demo.lut.query" — Pack in:  mp "handle", mp "keys" (array of i64),
//                                "$v"/"$probe" as above.
//                      Pack out: mp "values" (array, i64 per found key, nil
//                                per missing key), i64 "found", i64 "size",
//                                i64 "builds" (echo — lets a consumer prove
//                                zero rebuild without a second capability).
//                      Handle faults: "wrong_type" (rule 5), "stale_handle"
//                      (rule 3), "bad_handle" (malformed / id out of range),
//                      "unsupported_version" (handle "$v" not spoken).
//
//   "demo.lut.dump"  — Pack in:  mp "handle", "$v"/"$probe" as above.
//                      Pack out: bin "lut" — the canonical materialization
//                                (magic "XLUT" + u8 version + be32 count +
//                                count * (be64 key, be64 value)) — BYTE-
//                                DETERMINISTIC for a given content, i64
//                                "size", i64 "builds" (echo).
//                      Same handle faults as query.
//
// Contract failures are NORMAL sealed packs carrying the $fault entries
// (pack_contract convention) — the funnel rc stays 0; consumers route the
// $fault. Hard internal failures return XI_PACK_NULL.
//
// THREAD SAFETY: handlers arrive concurrently from multiple dispatch threads
// (the funnel does NOT serialize — the provider contract). Ring + dedup map
// are mutex-guarded; resolve hands out a shared_ptr copy so a concurrent
// recycle can never free an object under a running query/dump (the object
// dies when the last in-flight reader drops it — still inside this DLL,
// rule 1 intact). Counters are atomics.
//
#include <xi/xi_abi.hpp>
#include <xi/xi_json.hpp>
#include <xi/xi_mp.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr const char* kVersions   = "1";          // supported "$v", all three caps
constexpr const char* kTypeName   = "demo.lut";   // the handle namespace
constexpr int64_t     kHandleV    = 1;            // handle-entry schema version

// DLL-lifetime monotonic generation source. NEVER reset: instance reinit or
// destroy/recreate mints fresh generations, so a handle that survived an owner
// rebuild can only resolve STALE, never alias a new object (rule 3).
std::atomic<int64_t> g_gen_source{0};

// The heavy object: immutable after construction (rule 2).
struct Lut {
    std::vector<std::pair<int64_t, int64_t>> sorted;  // by key, unique keys

    const int64_t* find(int64_t key) const {
        auto it = std::lower_bound(sorted.begin(), sorted.end(), key,
                                   [](const auto& p, int64_t k) { return p.first < k; });
        if (it == sorted.end() || it->first != key) return nullptr;
        return &it->second;
    }
};

void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}
void put_be64(std::vector<uint8_t>& v, uint64_t x) {
    put_be32(v, (uint32_t)(x >> 32)); put_be32(v, (uint32_t)x);
}

} // namespace

class LutOwner : public xi::Plugin {
public:
    LutOwner(const xi_host_api* host, const std::string& name)
        : xi::Plugin(host, name) {
        pk_ = pack_iface();   // xi.pack@1 (cached by the base)
        if (host && host->get_interface) {
            provider_ = static_cast<const xi_cap_provider_v1*>(
                host->get_interface("xi.cap.provider", 1));
        }
        ring_.resize((size_t)ring_slots_);
        if (!pk_ || !provider_) {
            // Inert on a host without the planes (certify probes an older
            // table): loadable, but it provides nothing and says so.
            status("lut_owner: host lacks xi.pack@1/xi.cap.provider@1 — no capabilities registered");
            return;
        }
        int32_t r1 = provider_->register_capability("demo.lut.build", &h_build, this);
        int32_t r2 = provider_->register_capability("demo.lut.query", &h_query, this);
        int32_t r3 = provider_->register_capability("demo.lut.dump",  &h_dump,  this);
        registered_ = (r1 == XI_CAP_REG_OK && r2 == XI_CAP_REG_OK && r3 == XI_CAP_REG_OK);
        if (registered_) {
            status("lut_owner: providing demo.lut.build, demo.lut.query, demo.lut.dump");
        } else {
            char m[128];
            std::snprintf(m, sizeof(m),
                          "lut_owner: registration failed (build=%d query=%d dump=%d)",
                          r1, r2, r3);
            status(m);
        }
    }

    // Well-behaved lib: unregister on destroy (the host's adapter-dtor owner
    // sweep backstops a plugin that forgets). The ring's objects die here too
    // — rule 1: every free happens inside this DLL.
    ~LutOwner() override {
        if (provider_ && registered_) {
            provider_->unregister_capability("demo.lut.build", this);
            provider_->unregister_capability("demo.lut.query", this);
            provider_->unregister_capability("demo.lut.dump",  this);
        }
    }

    // -- control surface ------------------------------------------------------
    std::string exchange(const std::string& cmd) override {
        auto p = xi::Json::parse(cmd);
        const std::string command = p["command"].as_string();
        if (command == "stats") {
            size_t live = 0, slots = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                slots = ring_.size();
                for (const auto& s : ring_) if (s.obj) ++live;
            }
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                "{\"builds\":%lld,\"dedup_hits\":%lld,\"queries\":%lld,"
                "\"dumps\":%lld,\"recycles\":%lld,\"stale_faults\":%lld,"
                "\"wrong_type_faults\":%lld,\"live\":%zu,\"ring_slots\":%zu,"
                "\"registered\":%s}",
                (long long)builds_.load(), (long long)dedup_hits_.load(),
                (long long)queries_.load(), (long long)dumps_.load(),
                (long long)recycles_.load(), (long long)stale_faults_.load(),
                (long long)wrong_type_faults_.load(), live, slots,
                registered_ ? "true" : "false");
            return buf;
        }
        if (command == "recycle_all") {
            // The operator lever: drop every live object, bump generations.
            // Every outstanding handle goes stale (rule 3); in-flight readers
            // finish safely on their shared_ptr copies.
            int n = 0;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& s : ring_)
                    if (s.obj) { recycle_locked_(s); ++n; }
                dedup_.clear();
            }
            char buf[48];
            std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"recycled\":%d}", n);
            return buf;
        }
        return exchange_unknown_command(command);
    }

    std::string get_def() const override {
        std::lock_guard<std::mutex> lk(mu_);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "{\"ring_slots\":%zu}", ring_.size());
        return buf;
    }
    bool set_def(const std::string& json) override {
        auto p = xi::Json::parse(json);
        if (!p.valid()) return false;
        std::lock_guard<std::mutex> lk(mu_);
        int n = p["ring_slots"].as_int((int)ring_.size());
        if (n < 1) n = 1;
        if ((size_t)n < ring_.size()) {
            // Shrink: recycle the dropped tail (their objects are freed HERE,
            // rule 1); ids >= n resolve to bad_handle from now on.
            for (size_t i = (size_t)n; i < ring_.size(); ++i)
                if (ring_[i].obj) recycle_locked_(ring_[i]);
            rebuild_dedup_locked_((size_t)n);
        }
        ring_.resize((size_t)n);
        ring_slots_ = n;
        return true;
    }

private:
    // One ring slot: the leased residence of one immutable object (rule 3).
    struct Slot {
        std::shared_ptr<const Lut> obj;          // null = free
        int64_t  gen   = 0;                      // current lease generation
        uint64_t ckey  = 0;                      // content key (dedup)
        uint64_t touch = 0;                      // LRU clock stamp
    };

    // -- $v / $probe convention (imgcodec's gate, verbatim discipline) ---------
    xi_pack_handle version_gate_(xi_pack_handle in) const {
        int32_t probe = 0;
        if (pk_->get_bool && pk_->get_bool(in, "$probe", &probe) && probe) {
            xi_pack_builder b = pk_->builder_new();
            pk_->builder_add_str(b, "$versions", kVersions,
                                 (int32_t)std::strlen(kVersions));
            return pk_->builder_seal(b);
        }
        int64_t v = 1;                       // absent $v = the documented default
        pk_->get_i64(in, "$v", &v);
        if (v != 1) {
            xi_pack_builder b = pk_->builder_new();
            pk_->builder_add_str(b, "$fault", "unsupported_version", 19);
            pk_->builder_add_str(b, "$fault_key", "$v", 2);
            pk_->builder_add_str(b, "$versions", kVersions,
                                 (int32_t)std::strlen(kVersions));
            return pk_->builder_seal(b);
        }
        return XI_PACK_NULL;
    }

    xi_pack_handle fault_(const char* code, const char* key, const char* detail) const {
        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_str(b, "$fault", code, (int32_t)std::strlen(code));
        pk_->builder_add_str(b, "$fault_key", key, (int32_t)std::strlen(key));
        pk_->builder_add_str(b, "$fault_detail", detail, (int32_t)std::strlen(detail));
        return pk_->builder_seal(b);
    }

    // -- mp helpers -------------------------------------------------------------
    // Read one canonical mp entry as an array of i64. False on absent entry or
    // any non-int element (the pack plane guarantees well-formed canonical
    // bytes; SHAPE is still this capability's contract to check).
    bool read_i64_array_(xi_pack_handle in, const char* key,
                         std::vector<int64_t>* out) const {
        const void* p = nullptr; int32_t n = 0;
        if (!pk_->get_mp(in, key, &p, &n) || !p || n <= 0) return false;
        xi::mp::Reader r(static_cast<const uint8_t*>(p), (size_t)n);
        xi::mp::Element e;
        if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Array)
            return false;
        out->clear();
        out->reserve(e.len);
        for (uint32_t i = 0; i < e.len; ++i) {
            xi::mp::Element el;
            if (r.next(el) != xi::mp::Status::Ok) return false;
            if (el.kind == xi::mp::Kind::Int)       out->push_back(el.i);
            else if (el.kind == xi::mp::Kind::UInt) out->push_back((int64_t)el.u);
            else return false;
        }
        return true;
    }

    // The handle entry, canonical form. Field order is fixed (type, id, gen,
    // $v) so the entry bytes are deterministic for a given lease.
    void add_handle_(xi_pack_builder b, int64_t id, int64_t gen) const {
        xi::mp::Writer w;
        w.map(4);
        w.key("type"); w.str(kTypeName);
        w.key("id");   w.int_(id);
        w.key("gen");  w.int_(gen);
        w.key("$v");   w.int_(kHandleV);
        pk_->builder_add_mp(b, "handle", w.bytes().data(),
                            (int32_t)w.bytes().size());
    }

    // Parse + resolve a handle entry. On success returns the pinned object
    // (shared_ptr copy — safe against concurrent recycle) and sets *id/*gen.
    // On any contract violation returns null and *fault_out carries the sealed
    // $fault pack (rules 3 and 5 live here).
    std::shared_ptr<const Lut> resolve_(xi_pack_handle in, xi_pack_handle* fault_out) {
        *fault_out = XI_PACK_NULL;
        const void* p = nullptr; int32_t n = 0;
        if (!pk_->get_mp(in, "handle", &p, &n) || !p || n <= 0) {
            *fault_out = fault_("missing_input", "handle",
                                "demo.lut: required mp entry 'handle' is missing");
            return nullptr;
        }
        std::string type;
        int64_t id = -1, gen = -1, hv = kHandleV;
        bool have_id = false, have_gen = false;
        {
            xi::mp::Reader r(static_cast<const uint8_t*>(p), (size_t)n);
            xi::mp::Element e;
            if (r.next(e) != xi::mp::Status::Ok || e.kind != xi::mp::Kind::Map) {
                *fault_out = fault_("bad_handle", "handle",
                                    "demo.lut: handle entry is not an mp map");
                return nullptr;
            }
            for (uint32_t i = 0; i < e.len; ++i) {
                xi::mp::Element k, v;
                if (r.next(k) != xi::mp::Status::Ok || k.kind != xi::mp::Kind::Str ||
                    r.next(v) != xi::mp::Status::Ok) {
                    *fault_out = fault_("bad_handle", "handle",
                                        "demo.lut: malformed handle map");
                    return nullptr;
                }
                std::string key(reinterpret_cast<const char*>(k.data), k.len);
                const int64_t iv = (v.kind == xi::mp::Kind::Int)  ? v.i
                                 : (v.kind == xi::mp::Kind::UInt) ? (int64_t)v.u
                                                                  : 0;
                if (key == "type" && v.kind == xi::mp::Kind::Str)
                    type.assign(reinterpret_cast<const char*>(v.data), v.len);
                else if (key == "id")  { id = iv;  have_id = true; }
                else if (key == "gen") { gen = iv; have_gen = true; }
                else if (key == "$v")  { hv = iv; }
            }
        }
        if (hv != kHandleV) {
            *fault_out = fault_("unsupported_version", "handle",
                                "demo.lut: handle $v not spoken (supported: 1)");
            return nullptr;
        }
        if (type != kTypeName) {                              // rule 5
            wrong_type_faults_.fetch_add(1, std::memory_order_relaxed);
            char d[128];
            std::snprintf(d, sizeof(d),
                          "demo.lut: handle type '%s' does not belong to this owner",
                          type.empty() ? "(absent)" : type.c_str());
            *fault_out = fault_("wrong_type", "handle", d);
            return nullptr;
        }
        if (!have_id || !have_gen) {
            *fault_out = fault_("bad_handle", "handle",
                                "demo.lut: handle map lacks id/gen");
            return nullptr;
        }
        std::lock_guard<std::mutex> lk(mu_);
        if (id < 0 || (size_t)id >= ring_.size()) {
            *fault_out = fault_("bad_handle", "handle",
                                "demo.lut: handle id out of ring range");
            return nullptr;
        }
        Slot& s = ring_[(size_t)id];
        if (!s.obj || s.gen != gen) {                         // rule 3
            stale_faults_.fetch_add(1, std::memory_order_relaxed);
            char d[128];
            std::snprintf(d, sizeof(d),
                          "demo.lut: stale lease (id=%lld handle_gen=%lld ring_gen=%lld live=%d)",
                          (long long)id, (long long)gen, (long long)s.gen,
                          s.obj ? 1 : 0);
            *fault_out = fault_("stale_handle", "handle", d);
            return nullptr;
        }
        s.touch = ++clock_;
        return s.obj;                                          // pinned copy
    }

    // -- ring internals (mu_ held) ----------------------------------------------
    void recycle_locked_(Slot& s) {
        s.obj.reset();                                         // freed HERE (rule 1)
        s.gen  = g_gen_source.fetch_add(1, std::memory_order_relaxed) + 1;
        s.ckey = 0;
        recycles_.fetch_add(1, std::memory_order_relaxed);
    }
    void rebuild_dedup_locked_(size_t upto) {
        dedup_.clear();
        for (size_t i = 0; i < upto && i < ring_.size(); ++i)
            if (ring_[i].obj) dedup_[ring_[i].ckey] = (int32_t)i;
    }

    // -- demo.lut.build -----------------------------------------------------------
    xi_pack_handle build_(xi_pack_handle in) {
        if (xi_pack_handle early = version_gate_(in)) return early;

        std::vector<int64_t> keys, values;
        if (!read_i64_array_(in, "keys", &keys))
            return fault_("missing_input", "keys",
                          "demo.lut.build: required mp entry 'keys' (array of i64) is missing or malformed");
        if (!read_i64_array_(in, "values", &values))
            return fault_("missing_input", "values",
                          "demo.lut.build: required mp entry 'values' (array of i64) is missing or malformed");
        if (keys.empty() || keys.size() != values.size())
            return fault_("bad_input", "keys",
                          "demo.lut.build: keys/values must be non-empty and the same length");

        // Content identity: FNV-1a over the (key,value) stream — the dedup key.
        uint64_t ckey = 1469598103934665603ull;
        auto mix = [&ckey](int64_t x) {
            for (int i = 0; i < 8; ++i) {
                ckey ^= (uint8_t)(((uint64_t)x >> (i * 8)) & 0xFF);
                ckey *= 1099511628211ull;
            }
        };
        for (size_t i = 0; i < keys.size(); ++i) { mix(keys[i]); mix(values[i]); }

        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = dedup_.find(ckey);
            if (it != dedup_.end()) {
                Slot& s = ring_[(size_t)it->second];
                if (s.obj && s.ckey == ckey) {
                    // Same sealed content: same lease, zero rebuild.
                    s.touch = ++clock_;
                    dedup_hits_.fetch_add(1, std::memory_order_relaxed);
                    return build_reply_((int64_t)it->second, s.gen,
                                        (int64_t)s.obj->sorted.size(), /*built=*/0);
                }
                dedup_.erase(it);   // entry rotted (slot recycled) — rebuild below
            }
        }

        // Construct OUTSIDE the lock (the heavy part), then lease a slot.
        auto lut = std::make_shared<Lut>();
        lut->sorted.reserve(keys.size());
        for (size_t i = 0; i < keys.size(); ++i)
            lut->sorted.emplace_back(keys[i], values[i]);
        std::sort(lut->sorted.begin(), lut->sorted.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 1; i < lut->sorted.size(); ++i)
            if (lut->sorted[i].first == lut->sorted[i - 1].first)
                return fault_("bad_input", "keys",
                              "demo.lut.build: duplicate keys (a LUT is a function)");

        int64_t id, gen, size = (int64_t)lut->sorted.size();
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Lease: first free slot, else recycle the LRU (ring pressure IS
            // the forced-recycle path — outstanding handles to that slot go
            // stale, rule 3).
            size_t pick = ring_.size();
            for (size_t i = 0; i < ring_.size(); ++i)
                if (!ring_[i].obj) { pick = i; break; }
            if (pick == ring_.size()) {
                uint64_t best = UINT64_MAX;
                for (size_t i = 0; i < ring_.size(); ++i)
                    if (ring_[i].touch < best) { best = ring_[i].touch; pick = i; }
                dedup_.erase(ring_[pick].ckey);
                recycle_locked_(ring_[pick]);
            }
            Slot& s = ring_[pick];
            if (s.gen == 0)   // first lease of a virgin slot still gets a real gen
                s.gen = g_gen_source.fetch_add(1, std::memory_order_relaxed) + 1;
            s.obj   = std::move(lut);
            s.ckey  = ckey;
            s.touch = ++clock_;
            dedup_[ckey] = (int32_t)pick;
            id  = (int64_t)pick;
            gen = s.gen;
        }
        builds_.fetch_add(1, std::memory_order_relaxed);
        return build_reply_(id, gen, size, /*built=*/1);
    }

    xi_pack_handle build_reply_(int64_t id, int64_t gen, int64_t size, int64_t built) {
        xi_pack_builder b = pk_->builder_new();
        add_handle_(b, id, gen);
        pk_->builder_add_i64(b, "built", built);
        pk_->builder_add_i64(b, "builds", builds_.load(std::memory_order_relaxed));
        pk_->builder_add_i64(b, "size", size);
        return pk_->builder_seal(b);
    }

    // -- demo.lut.query -----------------------------------------------------------
    xi_pack_handle query_(xi_pack_handle in) {
        if (xi_pack_handle early = version_gate_(in)) return early;

        xi_pack_handle f = XI_PACK_NULL;
        auto lut = resolve_(in, &f);
        if (!lut) return f;

        std::vector<int64_t> keys;
        if (!read_i64_array_(in, "keys", &keys))
            return fault_("missing_input", "keys",
                          "demo.lut.query: required mp entry 'keys' (array of i64) is missing or malformed");

        xi::mp::Writer w;
        w.array((uint32_t)keys.size());
        int64_t found = 0;
        for (int64_t k : keys) {
            if (const int64_t* v = lut->find(k)) { w.int_(*v); ++found; }
            else                                 { w.nil(); }
        }
        queries_.fetch_add(1, std::memory_order_relaxed);

        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_mp(b, "values", w.bytes().data(), (int32_t)w.bytes().size());
        pk_->builder_add_i64(b, "found", found);
        pk_->builder_add_i64(b, "size", (int64_t)lut->sorted.size());
        pk_->builder_add_i64(b, "builds", builds_.load(std::memory_order_relaxed));
        return pk_->builder_seal(b);
    }

    // -- demo.lut.dump ------------------------------------------------------------
    // The MATERIALIZER (rule 4): the byte-deterministic canonical form a
    // persist sink stores instead of the runtime-only handle entry.
    xi_pack_handle dump_(xi_pack_handle in) {
        if (xi_pack_handle early = version_gate_(in)) return early;

        xi_pack_handle f = XI_PACK_NULL;
        auto lut = resolve_(in, &f);
        if (!lut) return f;

        std::vector<uint8_t> bytes;
        bytes.reserve(9 + lut->sorted.size() * 16);
        bytes.push_back('X'); bytes.push_back('L'); bytes.push_back('U'); bytes.push_back('T');
        bytes.push_back(1);                              // format version
        put_be32(bytes, (uint32_t)lut->sorted.size());
        for (const auto& [k, v] : lut->sorted) {
            put_be64(bytes, (uint64_t)k);
            put_be64(bytes, (uint64_t)v);
        }
        dumps_.fetch_add(1, std::memory_order_relaxed);

        xi_pack_builder b = pk_->builder_new();
        pk_->builder_add_bin(b, "lut", bytes.data(), (int32_t)bytes.size());
        pk_->builder_add_i64(b, "size", (int64_t)lut->sorted.size());
        pk_->builder_add_i64(b, "builds", builds_.load(std::memory_order_relaxed));
        return pk_->builder_seal(b);
    }

    // The registered pack-door-shaped handlers (funnel-invoked, concurrent).
    static xi_pack_handle h_build(void* self, xi_pack_handle in) {
        try { return static_cast<LutOwner*>(self)->build_(in); }
        catch (...) { return XI_PACK_NULL; }
    }
    static xi_pack_handle h_query(void* self, xi_pack_handle in) {
        try { return static_cast<LutOwner*>(self)->query_(in); }
        catch (...) { return XI_PACK_NULL; }
    }
    static xi_pack_handle h_dump(void* self, xi_pack_handle in) {
        try { return static_cast<LutOwner*>(self)->dump_(in); }
        catch (...) { return XI_PACK_NULL; }
    }

    const xi_pack_v1*         pk_       = nullptr;
    const xi_cap_provider_v1* provider_ = nullptr;
    bool                      registered_ = false;

    mutable std::mutex mu_;   // ring_ / dedup_ / clock_ / ring_slots_
    std::vector<Slot>  ring_;
    std::unordered_map<uint64_t, int32_t> dedup_;   // content key -> slot id
    uint64_t clock_      = 0;                       // LRU stamp source
    int      ring_slots_ = 8;

    std::atomic<int64_t> builds_{0};        // real constructions — the zero-rebuild proof
    std::atomic<int64_t> dedup_hits_{0};
    std::atomic<int64_t> queries_{0};
    std::atomic<int64_t> dumps_{0};
    std::atomic<int64_t> recycles_{0};
    std::atomic<int64_t> stale_faults_{0};
    std::atomic<int64_t> wrong_type_faults_{0};
};

XI_PLUGIN_IMPL(LutOwner)
