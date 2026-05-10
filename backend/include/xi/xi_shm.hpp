#pragma once
//
// xi_shm.hpp — opt-in shared-memory buffer pool that backend, plugins,
// and user scripts can all reach when they want to share image / byte
// data WITHOUT a memcpy.
//
// Design:
//
//   - One SHM region per backend process, named "xinsp2-shm-<pid>" by
//     default. Created by the backend at startup with a configurable
//     size (default ~1 GB).
//
//   - Anyone (in-proc or out-of-proc) opens the region by name and
//     mmaps it. After that, every party reads / writes the SAME bytes.
//
//   - Bump allocator on top of the region. Each allocation gets a
//     BlockHeader (cache-line) followed by payload. The handle returned
//     to callers is just the byte OFFSET of the BlockHeader within the
//     region — portable across processes since both parties mmap the
//     same backing object.
//
//   - Refcount lives in the BlockHeader as std::atomic<int32_t>. On
//     Windows / x86-64 std::atomic over a 32-bit int compiles to a
//     LOCK-prefixed instruction directly on the memory address, which
//     works equally well across processes mapping the same physical
//     page.
//
//   - This spike does NOT reclaim freed blocks (release just drops
//     refcount to 0). A production version would maintain a free-list /
//     size-class arena. Bump-only is enough to validate the cross-
//     process ownership model.
//
// Handle encoding:
//
//     uint64_t handle = byte offset of BlockHeader from region base.
//     Top 8 bits reserved for a magic tag so a stale handle from a
//     wrong region fails validation (caught in deref).
//
// This header is Windows-only for the spike (CreateFileMapping). The
// abstraction would extend to shm_open / mmap on POSIX with the same
// surface, deferred.
//

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace xi {

// 8-bit magic tag baked into every handle's top byte, so opening a
// region of a different generation / version makes old handles fail
// validation cleanly. Generation is per-region (set at create time).
constexpr uint8_t SHM_HANDLE_TAG = 0xA5;

// Compile-time cap on number of size-class buckets. Runtime config can
// declare anywhere from 1 to N_BUCKETS_MAX. Cap is small so per-bucket
// arrays (free_head[], a2_free_count[]) fit in a single cache line.
constexpr int N_BUCKETS_MAX = 8;

// Tier identifiers for ShmBlockHeader::tier_origin.
//   A1 = instance-private static pool (Phase D)
//   A2 = shared size-class free-list (Phase B)
//   A3 = bump fallback (existing path)
// A2 is the default for blocks alloc'd without an explicit owner_id.
constexpr uint8_t TIER_A1 = 0;
constexpr uint8_t TIER_A2 = 1;
constexpr uint8_t TIER_A3 = 2;

// Project-level allocator configuration. Read from project.json's
// `shm` block at open_project; passed to ShmRegion::create; persisted
// in ShmRegionHeader so the worker process reads identical values
// when it attaches.
//
// All fields are optional in project.json; defaults below applied
// per-key. Workers don't get to override — host is the source of
// truth.
struct ShmConfig {
    uint64_t region_size_bytes        = 512ull * 1024 * 1024;
    uint64_t promote_threshold_bytes  = 16ull  * 1024 * 1024;
    uint64_t bucket_sizes_bytes[N_BUCKETS_MAX] = {
        16ull  * 1024 * 1024,   //  16 MB
        64ull  * 1024 * 1024,   //  64 MB
        256ull * 1024 * 1024,   // 256 MB
        0, 0, 0, 0, 0
    };
    int32_t  n_buckets                  = 3;
    int32_t  max_in_flight_per_instance = 3;
};

// Cache-line-aligned counter block for production observability.
// Lives inside ShmRegionHeader. All fields are atomic so host /
// workers / monitoring tools can read without locking. Values are
// monotonic except a2_free_count[] which is the current outstanding-
// free count per bucket.
struct alignas(64) ShmMetrics {
    std::atomic<uint64_t> a1_acquire_total;        // Tier 1 hits
    std::atomic<uint64_t> a2_acquire_total;        // Tier 2 hits (free-list pop)
    std::atomic<uint64_t> a2_bump_inflate_total;   // Tier 2 free-list miss → bump as bucket-sized
    std::atomic<uint64_t> a3_bump_total;           // Tier 3 raw bump fallback (size > all buckets)
    std::atomic<uint64_t> alloc_failed_total;      // Region full → INVALID returned
    std::atomic<int32_t>  a2_free_count[N_BUCKETS_MAX];  // current depth of each bucket's free-list
    uint32_t              _pad[14];                // pad to 128 B (2 cache lines)
};
static_assert(sizeof(ShmMetrics) == 128, "ShmMetrics must be 128 bytes");

// Layout of byte 0 of the SHM region. The header is sized so the
// payload starts on a 64-byte boundary regardless of which fields
// land on which cache line.
//
// Layout v2 adds: ShmConfig (read-only, set at create), free_head[]
// (Phase B uses), ShmMetrics (Phase E uses). Phases B/E read/write
// these; Phase A only initialises them to zero so the layout exists.
//
// SHM_VERSION bumped to 2; cross-version handshake (PR #50) catches
// host/worker built from different commits.
struct alignas(64) ShmRegionHeader {
    // --- cache line 1: identification + bump state -----------------
    uint32_t                  magic;          // 'XSHM' = 0x4D485358
    uint32_t                  version;        // bump on layout change
    uint64_t                  total_size;     // bytes
    uint64_t                  payload_start;  // first byte after this header
    std::atomic<uint64_t>     bump_offset;    // next free, cross-proc atomic
    std::atomic<uint32_t>     block_count;
    uint8_t                   tag;            // baked into every handle
    uint8_t                   _pad_a[3];
    uint32_t                  _pad_b[5];      // pad to 64

    // --- cache line 2: ShmConfig snapshot (read-only after create) -
    uint64_t                  cfg_promote_threshold_bytes;
    int32_t                   cfg_n_buckets;
    int32_t                   cfg_max_in_flight_per_instance;
    uint64_t                  cfg_bucket_sizes_bytes[N_BUCKETS_MAX]; // 64 B
    uint32_t                  _pad_c[2];                              // pad to 128

    // --- cache lines 3-4: A2 free-list heads (Phase B) -------------
    // Encoded as (tag : u16, offset : u48). offset==0 means empty.
    // ABA defense: tag monotonically incremented per push.
    std::atomic<uint64_t>     free_head[N_BUCKETS_MAX];               // 64 B (1 cache line)
    uint8_t                   _pad_d[64];                             // reserve cache line for A1 future

    // --- cache lines 5-6: metrics (Phase E) ------------------------
    ShmMetrics                metrics;                                 // 128 B
};
static_assert(sizeof(ShmRegionHeader) % 64 == 0,
              "ShmRegionHeader must be a whole number of cache lines");

// Per-block metadata sitting in front of the payload.
//
// Layout v2 (still 64 bytes; new fields carved out of _pad):
//   stride            — image row pitch in bytes (aligned to kRowAlign)
//   bucket_index      — cached bucket index for O(1) release dispatch
//   tier_origin       — TIER_A1 / A2 / A3 (sets release destination)
//   owner_instance_id — non-zero only for Tier 1 blocks
//   next_free_offset  — populated when block is on a free-list
struct alignas(64) ShmBlockHeader {
    uint32_t              magic;             // 'XBLK' = 0x4B4C4258
    uint32_t              kind;              // 0=image, 1=buffer
    int32_t               width;
    int32_t               height;
    int32_t               channels;
    int32_t               payload_size;      // bytes (raw payload, not including header)
    int32_t               stride;            // image row pitch; 0 for buffers
    int32_t               bucket_index;      // -1 if not a bucket-allocated block (legacy / future)
    uint8_t               tier_origin;       // TIER_A1 / A2 / A3
    uint8_t               _pad_a[3];
    int32_t               owner_instance_id; // 0 for shared (A2/A3) blocks
    uint64_t              next_free_offset;  // free-list link when refcount=0
    std::atomic<int32_t>  refcount;
    uint32_t              _reserved[3];      // pad to 64 bytes
};
static_assert(sizeof(ShmBlockHeader) == 64, "BlockHeader must be 64 bytes");

constexpr uint32_t SHM_REGION_MAGIC = 0x4D485358; // 'XSHM' little-endian
constexpr uint32_t SHM_BLOCK_MAGIC  = 0x4B4C4258; // 'XBLK' little-endian
constexpr uint32_t SHM_VERSION      = 2;          // bumped from 1 in Phase A

// Opaque RAII handle to one mapped SHM region.
class ShmRegion {
public:
    static constexpr uint64_t INVALID_HANDLE = 0;

    enum class Kind : uint32_t {
        Image  = 0,
        Buffer = 1,
    };

    // Create a fresh region with a ShmConfig (Phase A). Old single-
    // argument signature is kept as an inline shim so existing callers
    // build clean while we roll out config plumbing.
    static ShmRegion create(const std::string& name, uint64_t total_bytes) {
        ShmConfig cfg;
        cfg.region_size_bytes = total_bytes;
        return create(name, cfg);
    }

    // Create a fresh region from a ShmConfig. Region size comes from
    // cfg.region_size_bytes (rounded to page granularity). Bucket
    // sizes / promote threshold / max_in_flight are persisted in the
    // header so the worker `attach()` reads identical values.
    // Throws on failure. The OS object lives until every mapping closes.
    static ShmRegion create(const std::string& name, const ShmConfig& cfg) {
#ifdef _WIN32
        // Round up to a page boundary so we don't waste tail bytes.
        SYSTEM_INFO si; GetSystemInfo(&si);
        uint64_t pg = si.dwAllocationGranularity;
        uint64_t sz = ((cfg.region_size_bytes + pg - 1) / pg) * pg;

        HANDLE map = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            (DWORD)(sz >> 32), (DWORD)(sz & 0xFFFFFFFF),
            name.c_str());
        if (!map) throw std::runtime_error("CreateFileMappingA failed");

        // Note: GetLastError() == ERROR_ALREADY_EXISTS means someone
        // beat us to it — that's "attach" semantics, not "create".
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(map);
            throw std::runtime_error("SHM region '" + name + "' already exists");
        }

        void* base = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sz);
        if (!base) {
            CloseHandle(map);
            throw std::runtime_error("MapViewOfFile failed");
        }

        ShmRegion r;
        r.name_      = name;
        r.map_       = map;
        r.base_      = base;
        r.total_size_= sz;
        r.owner_     = true;

        // Initialise the region header. Bump offset starts right after
        // the header, page-aligned for safety.
        auto* h = r.header();
        std::memset(h, 0, sizeof(*h));
        h->magic         = SHM_REGION_MAGIC;
        h->version       = SHM_VERSION;
        h->total_size    = sz;
        h->payload_start = ((sizeof(ShmRegionHeader) + 63) / 64) * 64;
        h->bump_offset.store(h->payload_start, std::memory_order_release);
        h->block_count.store(0, std::memory_order_relaxed);
        h->tag           = SHM_HANDLE_TAG;

        // Persist ShmConfig so worker's attach() reads consistent
        // values. Worker MUST NOT override these — host is source of
        // truth. Bucket / metric arrays were memset to 0 above; only
        // the configured ones get touched in subsequent phases.
        h->cfg_promote_threshold_bytes    = cfg.promote_threshold_bytes;
        h->cfg_n_buckets                  = cfg.n_buckets;
        h->cfg_max_in_flight_per_instance = cfg.max_in_flight_per_instance;
        for (int i = 0; i < N_BUCKETS_MAX; ++i) {
            h->cfg_bucket_sizes_bytes[i] = cfg.bucket_sizes_bytes[i];
        }
        return r;
#else
        (void)name; (void)cfg;
        throw std::runtime_error("xi_shm only implemented for Windows in this spike");
#endif
    }

    // Attach to an existing region by name (e.g. from a child process).
    static ShmRegion attach(const std::string& name) {
#ifdef _WIN32
        HANDLE map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!map) throw std::runtime_error("OpenFileMappingA failed");

        // Map a small piece first to read the header's total_size, then
        // remap with the full size. Saves us from passing size out-of-
        // band on every attach.
        void* peek = MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(ShmRegionHeader));
        if (!peek) {
            CloseHandle(map);
            throw std::runtime_error("MapViewOfFile peek failed");
        }
        auto* peek_hdr = reinterpret_cast<ShmRegionHeader*>(peek);
        if (peek_hdr->magic != SHM_REGION_MAGIC) {
            UnmapViewOfFile(peek); CloseHandle(map);
            throw std::runtime_error("region magic mismatch");
        }
        // C-P1-1: reject mismatched layout versions. Without this, a
        // host built from one commit and a worker built from another
        // (different ShmBlockHeader layout, different SHM_VERSION)
        // would silently corrupt the region — both sides write valid-
        // looking blocks against incompatible offsets. The version
        // check makes the mismatch fail fast at attach, so the worker
        // exits cleanly and the host's accept_one times out (now
        // bounded by P0-C1's timeout).
        if (peek_hdr->version != SHM_VERSION) {
            uint32_t got = peek_hdr->version;
            UnmapViewOfFile(peek); CloseHandle(map);
            throw std::runtime_error(
                "SHM version mismatch: region is v"
                + std::to_string(got)
                + " but this binary expects v"
                + std::to_string(SHM_VERSION)
                + " (host and worker built from different commits?)");
        }
        uint64_t sz = peek_hdr->total_size;
        UnmapViewOfFile(peek);

        void* base = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sz);
        if (!base) {
            CloseHandle(map);
            throw std::runtime_error("MapViewOfFile full failed");
        }

        ShmRegion r;
        r.name_      = name;
        r.map_       = map;
        r.base_      = base;
        r.total_size_= sz;
        r.owner_     = false;
        return r;
#else
        (void)name;
        throw std::runtime_error("xi_shm only implemented for Windows in this spike");
#endif
    }

    ShmRegion() = default;
    ShmRegion(const ShmRegion&) = delete;
    ShmRegion& operator=(const ShmRegion&) = delete;
    ShmRegion(ShmRegion&& o) noexcept { *this = std::move(o); }
    ShmRegion& operator=(ShmRegion&& o) noexcept {
        if (this != &o) {
            close_();
            name_       = std::move(o.name_);
            map_        = o.map_;       o.map_  = nullptr;
            base_       = o.base_;      o.base_ = nullptr;
            total_size_ = o.total_size_; o.total_size_ = 0;
            owner_      = o.owner_;     o.owner_ = false;
        }
        return *this;
    }
    ~ShmRegion() { close_(); }

    bool valid() const { return base_ != nullptr; }
    const std::string& name() const { return name_; }
    uint64_t total_size() const { return total_size_; }
    uint64_t used_bytes() const {
        return header()->bump_offset.load(std::memory_order_acquire)
             - header()->payload_start;
    }
    uint32_t block_count() const {
        return header()->block_count.load(std::memory_order_acquire);
    }

    // Allocate an image block. Refcount starts at 1. Returns
    // INVALID_HANDLE on out-of-space.
    //
    // P0-D2: payload_size is int32. A 50000x50000x4 image has
    // pixels=1e10 which truncates to a much smaller int32 — the
    // bump allocator would carve out a small block but the caller
    // would then write w*h*ch bytes, scribbling past the block end
    // into the next block's payload. Reject anything that doesn't
    // fit in int32 at the API boundary.
    //
    // Phase A: optional `owner_id` parameter; non-zero binds the
    // block to a Tier 1 instance pool (enforced in Phase D).
    // Default 0 = shared (Tier 2 / 3). Today (Phase A only),
    // owner_id is recorded in the header but doesn't change which
    // pool the block comes from — bump-only behaviour preserved.
    uint64_t alloc_image(int32_t w, int32_t h, int32_t ch,
                         int32_t owner_id = 0) {
        if (w <= 0 || h <= 0 || ch <= 0) return INVALID_HANDLE;
        const int64_t pixels = int64_t(w) * int64_t(h) * int64_t(ch);
        if (pixels <= 0 || pixels > INT32_MAX) return INVALID_HANDLE;
        // Phase A placeholder: stride := w*ch (no row alignment yet).
        // Phase B bumps to align_up(w*ch, kRowAlign).
        int32_t stride = w * ch;
        return alloc_(Kind::Image, w, h, ch, (int32_t)pixels,
                      stride, owner_id);
    }

    // Allocate an opaque byte buffer (e.g. ML weights, big metadata).
    uint64_t alloc_buffer(int32_t size_bytes,
                          int32_t owner_id = 0) {
        if (size_bytes <= 0) return INVALID_HANDLE;
        return alloc_(Kind::Buffer, 0, 0, 0, size_bytes,
                      /*stride=*/0, owner_id);
    }

    // Read-only view of a block's header fields. Used by callers that
    // need to know stride / tier_origin / owner_instance_id (e.g.
    // image_data wrappers, telemetry, Phase B/D dispatch).
    // Returns nullptr if the handle is invalid.
    const ShmBlockHeader* block_header(uint64_t handle) const {
        return block_(handle);
    }

    // Increment the refcount. Caller is responsible for matching with
    // a release(). Cross-process safe.
    void addref(uint64_t handle) {
        auto* b = block_(handle);
        if (!b) return;
        b->refcount.fetch_add(1, std::memory_order_relaxed);
    }

    // Decrement refcount. Returns the new refcount (0 means caller
    // dropped the last reference). Memory is not reclaimed in the
    // spike — the bump allocator never moves backwards.
    int32_t release(uint64_t handle) {
        auto* b = block_(handle);
        if (!b) return -1;
        int32_t prev = b->refcount.fetch_sub(1, std::memory_order_acq_rel);
        return prev - 1;
    }

    int32_t refcount(uint64_t handle) const {
        auto* b = block_(handle);
        return b ? b->refcount.load(std::memory_order_acquire) : -1;
    }

    // Pointer to the payload bytes for this handle. Same address space
    // semantics as any local buffer — read / write directly.
    uint8_t* data(uint64_t handle) {
        auto* b = block_(handle);
        if (!b) return nullptr;
        return reinterpret_cast<uint8_t*>(b) + sizeof(ShmBlockHeader);
    }
    const uint8_t* data(uint64_t handle) const {
        auto* b = block_(handle);
        if (!b) return nullptr;
        return reinterpret_cast<const uint8_t*>(b) + sizeof(ShmBlockHeader);
    }

    int32_t width   (uint64_t h) const { auto* b = block_(h); return b ? b->width    : 0; }
    int32_t height  (uint64_t h) const { auto* b = block_(h); return b ? b->height   : 0; }
    int32_t channels(uint64_t h) const { auto* b = block_(h); return b ? b->channels : 0; }
    int32_t size    (uint64_t h) const { auto* b = block_(h); return b ? b->payload_size : 0; }
    Kind    kind    (uint64_t h) const { auto* b = block_(h);
                                         return b ? (Kind)b->kind : Kind::Buffer; }

    bool is_valid_handle(uint64_t handle) const { return block_(handle) != nullptr; }

private:
    ShmRegionHeader* header() {
        return reinterpret_cast<ShmRegionHeader*>(base_);
    }
    const ShmRegionHeader* header() const {
        return reinterpret_cast<const ShmRegionHeader*>(base_);
    }

    // Decode handle → block pointer. Validates magic + bounds + tag.
    ShmBlockHeader* block_(uint64_t handle) const {
        if (!base_ || handle == INVALID_HANDLE) return nullptr;
        uint8_t tag = (uint8_t)(handle >> 56);
        uint64_t off = handle & 0x00FFFFFFFFFFFFFFull;
        const auto* h = header();
        if (tag != h->tag) return nullptr;
        if (off + sizeof(ShmBlockHeader) > total_size_) return nullptr;
        auto* b = reinterpret_cast<ShmBlockHeader*>((uint8_t*)base_ + off);
        if (b->magic != SHM_BLOCK_MAGIC) return nullptr;
        return b;
    }

    uint64_t alloc_(Kind k, int32_t w, int32_t h, int32_t ch,
                    int32_t payload_size,
                    int32_t stride        = 0,
                    int32_t owner_id      = 0) {
        if (!base_) return INVALID_HANDLE;
        auto* hdr = header();
        // Round up so each block's payload starts cache-aligned too.
        uint64_t total = sizeof(ShmBlockHeader) + (uint64_t)payload_size;
        total = ((total + 63) / 64) * 64;

        // CAS-bump the offset.
        uint64_t prev = hdr->bump_offset.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t next = prev + total;
            if (next > total_size_) return INVALID_HANDLE;
            if (hdr->bump_offset.compare_exchange_weak(prev, next,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) break;
        }
        auto* blk = reinterpret_cast<ShmBlockHeader*>((uint8_t*)base_ + prev);
        std::memset(blk, 0, sizeof(*blk));
        blk->magic             = SHM_BLOCK_MAGIC;
        blk->kind              = (uint32_t)k;
        blk->width             = w;
        blk->height            = h;
        blk->channels          = ch;
        blk->payload_size      = payload_size;
        blk->stride            = stride;
        // Phase A: every alloc_() goes through bump (no free-list yet),
        // so origin is "A2 bump-inflated" — the closest tier label
        // before Phase B introduces the actual size-class free-list.
        // Phase B will set tier_origin = TIER_A2 for free-list blocks
        // and TIER_A3 for true bump fallback.
        blk->bucket_index      = -1;
        blk->tier_origin       = TIER_A2;
        blk->owner_instance_id = owner_id;
        blk->next_free_offset  = 0;
        blk->refcount.store(1, std::memory_order_release);
        hdr->block_count.fetch_add(1, std::memory_order_relaxed);
        return ((uint64_t)hdr->tag << 56) | prev;
    }

    void close_() {
#ifdef _WIN32
        if (base_) { UnmapViewOfFile(base_); base_ = nullptr; }
        if (map_)  { CloseHandle(map_); map_ = nullptr; }
#endif
    }

    std::string name_;
    HANDLE      map_ = nullptr;
    void*       base_ = nullptr;
    uint64_t    total_size_ = 0;
    bool        owner_ = false;
};

} // namespace xi
