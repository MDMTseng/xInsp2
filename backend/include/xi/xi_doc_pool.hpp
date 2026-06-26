#pragma once
//
// xi_doc_pool.hpp — host-owned pooled allocator behind the γ in-process doc
// path (host_api.doc_chunk_*). Replaces the plain malloc/realloc/free wiring
// with a reuse pool so building/freeing yyjson docs on the hot path costs no
// per-frame malloc churn after warm-up. Pure internal swap — no ABI change,
// no caller change; doc_chunk_* keep their (size)/(ptr,size)/(ptr) signatures.
//
// Design (see docs/internals/data-layer.md §γ):
//   - Segregated free-lists by power-of-two size class (O(1) acquire/release,
//     no search). yyjson bump-allocates nodes WITHIN a chunk, so the pool is
//     hit only per chunk (a handful per doc), not per node.
//   - THREAD-LOCAL free-lists ⇒ no lock on the hot path. A block freed on a
//     different thread than it was allocated simply lands in the freeing
//     thread's list and is reused there — correct (a block is fungible memory
//     of its size class), just not perfectly balanced. Most docs are built and
//     freed on the same dispatch thread, so this is the common, lock-free case.
//   - Each block carries a 16-byte header (size class + requested size) so
//     free()/realloc() — which yyjson calls WITHOUT a size — can find the class.
//   - Oversize (> kMaxBlock) requests bypass the pool (plain malloc/free).
//
// Cross-DLL: a doc built through this allocator stores these fn pointers in
// doc->alc, so freeing it (from either side, under the project's /MD shared
// CRT) routes back here. The pool is a backend singleton-per-thread.
//
// TODO(perf): if cross-thread alloc/free imbalance shows up under heavy parallel
// dispatch, add periodic rebalancing or a shared overflow list. Not needed yet.
//

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace xi {

class DocChunkPool {
public:
    // Smallest / largest pooled block (TOTAL size incl. header). Requests whose
    // header+payload exceed kMaxBlock go straight to malloc.
    static constexpr size_t kHeader   = 16;     // keeps the returned ptr 16-aligned
    static constexpr size_t kMinBlock = 64;
    static constexpr size_t kMaxBlock = 1u << 16;   // 64 KiB
    static constexpr int    kNumClasses = 11;       // 64, 128, ... 65536

    static void* alloc(size_t size) {
        size_t total = kHeader + size;
        int c = class_for_(total);
        if (c < 0) {                                // oversize → direct malloc
            void* blk = std::malloc(total);
            if (!blk) return nullptr;
            write_header_(blk, -1, size);
            return user_ptr_(blk);
        }
        void* blk = pop_(c);
        if (!blk) {
            blk = std::malloc(class_size_(c));
            if (!blk) return nullptr;
        }
        write_header_(blk, c, size);
        return user_ptr_(blk);
    }

    static void* realloc(void* p, size_t size) {
        if (!p) return alloc(size);
        if (size == 0) { free(p); return nullptr; }
        void* blk = block_ptr_(p);
        int c = header_class_(blk);
        size_t cap = (c < 0) ? header_size_(blk)        // oversize: exact payload
                             : class_size_(c) - kHeader; // pooled: class payload
        if (size <= cap) {                               // fits where it is
            set_header_size_(blk, size);
            return p;
        }
        void* np = alloc(size);
        if (!np) return nullptr;
        size_t old = header_size_(blk);
        std::memcpy(np, p, old < size ? old : size);
        free(p);
        return np;
    }

    static void free(void* p) {
        if (!p) return;
        void* blk = block_ptr_(p);
        int c = header_class_(blk);
        if (c < 0) { std::free(blk); return; }   // oversize
        push_(c, blk);
    }

private:
    // Header lives at the start of every block: [int32 cls][uint32 size][pad].
    // When a block is FREE, its first 8 bytes are reused as the free-list link
    // (the header is rewritten on the next alloc), so no extra metadata memory.
    struct Header { int32_t cls; uint32_t size; };

    static void*  user_ptr_(void* blk)  { return static_cast<char*>(blk) + kHeader; }
    static void*  block_ptr_(void* p)   { return static_cast<char*>(p) - kHeader; }
    static void   write_header_(void* blk, int cls, size_t size) {
        auto* h = static_cast<Header*>(blk); h->cls = cls; h->size = (uint32_t)size;
    }
    static int    header_class_(void* blk) { return static_cast<Header*>(blk)->cls; }
    static size_t header_size_(void* blk)  { return static_cast<Header*>(blk)->size; }
    static void   set_header_size_(void* blk, size_t s) { static_cast<Header*>(blk)->size = (uint32_t)s; }

    static int class_for_(size_t total) {
        if (total > kMaxBlock) return -1;
        size_t b = kMinBlock; int c = 0;
        while (b < total) { b <<= 1; ++c; }
        return c;                                  // 0..kNumClasses-1
    }
    static size_t class_size_(int c) { return kMinBlock << c; }

    // Thread-local free-list heads, one per size class. Intrusive: a free
    // block's first sizeof(void*) bytes hold the next-free pointer.
    //
    // Wrapped in a struct with a destructor so the pooled blocks are reclaimed
    // when the OWNING THREAD EXITS. The dispatch model spawns a fresh detached
    // thread per cmd:run / one-shot inspect, so without this the whole doc's
    // chunk footprint leaked on every run (and every worker's high-water on every
    // start/stop). The dtor frees only blocks sitting in the free-list (i.e. ones
    // already returned to the pool) — live docs aren't on it, so no double-free.
    struct Heads {
        void* h[kNumClasses] = {};
        ~Heads() {
            for (int c = 0; c < kNumClasses; ++c) {
                void* blk = h[c];
                while (blk) { void* next = *static_cast<void**>(blk); std::free(blk); blk = next; }
            }
        }
    };
    static void** free_heads_() {
        static thread_local Heads heads;
        return heads.h;
    }
    static void* pop_(int c) {
        void** heads = free_heads_();
        void* blk = heads[c];
        if (blk) heads[c] = *static_cast<void**>(blk);
        return blk;
    }
    static void push_(int c, void* blk) {
        void** heads = free_heads_();
        *static_cast<void**>(blk) = heads[c];
        heads[c] = blk;
    }
};

} // namespace xi
