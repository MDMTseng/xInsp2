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

// Layout of byte 0 of the SHM region. Cache-line padded so the bump
// pointer doesn't share a line with metadata.
struct alignas(64) ShmRegionHeader {
    uint32_t                  magic;          // 'XSHM' = 0x4D485358
    uint32_t                  version;        // bump on layout change
    uint64_t                  total_size;     // bytes
    uint64_t                  payload_start;  // first byte after this header
    std::atomic<uint64_t>     bump_offset;    // next free, cross-proc atomic
    std::atomic<uint32_t>     block_count;
    uint8_t                   tag;            // baked into every handle
    uint8_t                   _pad[63 - sizeof(std::atomic<uint64_t>)
                                        - sizeof(std::atomic<uint32_t>)
                                        - sizeof(uint64_t)*2 - sizeof(uint32_t)*2 - 1];
};

// Per-block metadata sitting in front of the payload.
struct alignas(64) ShmBlockHeader {
    uint32_t              magic;     // 'XBLK' = 0x4B4C4258
    uint32_t              kind;      // 0=image, 1=buffer
    int32_t               width;
    int32_t               height;
    int32_t               channels;
    int32_t               payload_size;  // bytes
    std::atomic<int32_t>  refcount;
    uint32_t              _pad[8];   // pad to 64 bytes
};
static_assert(sizeof(ShmBlockHeader) == 64, "BlockHeader must be 64 bytes");

constexpr uint32_t SHM_REGION_MAGIC = 0x4D485358; // 'XSHM' little-endian
constexpr uint32_t SHM_BLOCK_MAGIC  = 0x4B4C4258; // 'XBLK' little-endian
constexpr uint32_t SHM_VERSION      = 1;

// Opaque RAII handle to one mapped SHM region.
class ShmRegion {
public:
    static constexpr uint64_t INVALID_HANDLE = 0;

    enum class Kind : uint32_t {
        Image  = 0,
        Buffer = 1,
    };

    // Create a fresh region, sized for `total_bytes` (rounded to page).
    // Throws on failure. The OS object lives until every mapping closes.
    static ShmRegion create(const std::string& name, uint64_t total_bytes) {
#ifdef _WIN32
        // Round up to a page boundary so we don't waste tail bytes.
        SYSTEM_INFO si; GetSystemInfo(&si);
        uint64_t pg = si.dwAllocationGranularity;
        uint64_t sz = ((total_bytes + pg - 1) / pg) * pg;

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
        return r;
#else
        (void)name; (void)total_bytes;
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
    uint64_t alloc_image(int32_t w, int32_t h, int32_t ch) {
        const int64_t pixels = int64_t(w) * h * ch;
        if (pixels <= 0) return INVALID_HANDLE;
        return alloc_(Kind::Image, w, h, ch, (int32_t)pixels);
    }

    // Allocate an opaque byte buffer (e.g. ML weights, big metadata).
    uint64_t alloc_buffer(int32_t size_bytes) {
        if (size_bytes <= 0) return INVALID_HANDLE;
        return alloc_(Kind::Buffer, 0, 0, 0, size_bytes);
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

    uint64_t alloc_(Kind k, int32_t w, int32_t h, int32_t ch, int32_t payload_size) {
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
        blk->magic        = SHM_BLOCK_MAGIC;
        blk->kind         = (uint32_t)k;
        blk->width        = w;
        blk->height       = h;
        blk->channels     = ch;
        blk->payload_size = payload_size;
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
