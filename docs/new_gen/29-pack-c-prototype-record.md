# 29 — Pack Design-C Prototype: Experiment Record

Status: **closed experiment, harvested.** The prototype header
`backend/tests/proto/xi_pack_c.hpp` is a frozen design-C record kept in the
test/bench tree, **out of `include/`**. This doc is its writeup: what design C
proved, the numbers it proved it with, what was lifted into production, and the
one caveat that keeps it from ever shipping as-is.

Companion to doc 28 (packv3 review findings — this closes finding ⑥) and doc 07
(uniform keyed-buffer plane). The production pack that came out of this
experiment is `xi_pack.hpp`.

---

## 1. What design C was

Design C was a candidate next-gen pack representation, built **to be
benchmarked** against the then-current `xi_pack.hpp` Pack/PackBuilder — never to
be integrated. Everything lived in one self-contained header:

> One pack = ONE contiguous **SLAB** + N external typed buffers.

- **Slab layout:** 64B header → fixed 32B directory entries (`key_hash`-sorted,
  binary search) → a bump-allocated, per-entry-aligned payload region. INLINE
  entries store scalars **raw** (i64/f64 = 8 bytes, read is a pointer-add + load,
  zero decode); strings raw; small structured trees as opaque canonical-msgpack
  bytes.
- **EXTERN entries** (tensors / large blobs): 64B-aligned buffers from a
  **size-class pool** (2ⁿ classes, 4 KiB…64 MiB) with a per-thread magazine (a
  small LIFO cache per class) over a global overflow shelf. Refcounted, carrying
  `{type_id, shape[3], bytes}`, resolved through a slot+generation handle table
  (the ImagePool discipline).
- **serialize** = slab verbatim + each EXTERN buffer appended; **deserialize**
  re-mints the EXTERN buffers and patches the directory handles in a fresh slab
  copy.
- RAII refs (`PackRef`/`BufRef`) as the C++-side ownership default; raw handle
  APIs as the C-ABI wire shape.

## 2. What it proved — the bench

Head-to-head harness: `backend/tests/bench_pack_c.cpp` (built but **not**
perf-gated — no baseline; the numbers feed a migrate/don't-migrate judgment, not
CI). Correctness net: `backend/tests/test_pack_c.cpp` (ctest `pack_c`), which
guards the prototype's claims so the numbers mean something.

The figures below are one representative **Release** run on the dev machine.
Absolute ns and the exact ratios swing with machine, allocator, and warm/cold
state — read the **shape**, not the third digit. `cur/C > 1` means design C is
faster.

| Lane                                       | current | design-C | cur/C |
|--------------------------------------------|--------:|---------:|------:|
| build+seal+drop (repr. ~7.9 MB pack)       | 576 µs  | 211 µs   | 2.7×  |
| meta read, 10 keys (current direct)        | 105 ns  | 92 ns    | 1.1×  |
| meta read, 10 keys (**current ABI trampoline**) | 197 ns | 92 ns | 2.1×  |
| tensor resolve+view ×2                     | 21 ns   | 26 ns    | 0.8×  |
| big-buffer alloc/free 1920×1200 (see note) | 53.6 µs | 37 ns    | ~1450× |
| serialize (repr. pack)                     | 1185 µs | 1216 µs  | 0.97× |
| deserialize+drop (repr. pack)              | 610 µs  | 213 µs   | 2.9×  |

Wire size was parity (≈7.81 MB both sides — the payload is the same ~7.9 MB of
pixels/blob on both).

**Reading the lanes:**
- **Buffer alloc/free is the headline.** The size-class magazine returns cached,
  *uninitialized* memory; the production ImagePool `create()` heap-allocated a
  `PoolEntry` and zero-filled a 2.3 MB `vector` (alloc + zero-fill + first-touch
  page faults) every cycle. That is an inherent, un-equalized asymmetry — and it
  is exactly design C's pitch. The ratio reads in the thousands and is
  allocator-sensitive; the *point* is orders of magnitude, not a stable number.
- **Build and deserialize** win ~2–3× on the slab: one recycled block + a bump
  allocator vs per-entry container churn, and re-minting buffers from the
  magazine on the way in.
- **Metadata reads** win modestly direct-to-direct, and ~2× once the current
  path pays its **real ABI cost** (the `xi_pack_v1` fn pointers do a
  `PackRegistry` shared-lock resolve per `get_*`). Design C's O(log n) directory
  binary-search reads off a raw pointer.
- **Serialize is parity** (~0.97×): slab-verbatim memcpy vs the record-plugin
  manual walk — both bounded by moving the same bytes.
- **Tensor resolve** is a wash (0.8×), within noise.

## 3. What was harvested

The experiment was not integrated wholesale; its two load-bearing wins were
lifted into production and the prototype retired:

1. **The pixpool size-class recycler → production `xi_image_pool.hpp`.** The
   magazine/size-class discipline that produced the buffer-alloc win was
   backported to the real ImagePool: `create()`/`release()` no longer pay
   `new PoolEntry` + `vector::resize` per cycle. Landed in **commit `cac9975`**
   ("Merge perf/imagepool-sizeclass: 10× image create/release via size-class
   recycling"). See the `pixpool` block in `xi_image_pool.hpp` and doc
   `docs/internals/pack-plane.md`.
2. **The slab migration itself → production `xi_pack.hpp`.** The v3 uniform
   keyed-buffer pack is the slab representation this experiment validated;
   `xi_pack.hpp`'s header still cites the proto as its validation record.

## 4. Why the header left `include/` (finding ⑥)

Once harvested, 1288 lines of non-production code sitting in the shipping include
tree was a liability, not an asset (doc 28 ⑥, charter value 6 膨脹再收縮):

- It carries **divergent semantics** a reader can mistake for a second real pack
  API — `key_hash`-order directory iteration (which **loses insertion order**),
  its own dtype id space (u8/u16/i32/f32/f64), and a slab-verbatim `serialize()`
  that is a *different* wire than `xi_pack.hpp`'s.
- Its **`deserialize()` is intentionally unhardened**: it validates the header
  only and does **no per-entry bounds checks** — the directory `off`/`len`/
  `key_off` fields are trusted. That makes it an unbounded-OOB deserializer on
  hostile bytes. Harmless where it lives now (fixed test/bench inputs), genuinely
  dangerous if ever promoted. **Never promote it without a full hardening pass.**

So the header moved to `backend/tests/proto/xi_pack_c.hpp` (test/bench-only,
never in `include/`), carrying a loud EXPERIMENT-RECORD banner, and its record
was lifted here. The correctness test keeps running from the new home (ctest
`pack_c`), and the head-to-head bench (`bench_pack_c`) still builds so the
numbers above stay reproducible.
