# Cross-process refcounted SHM allocator — formal specification

Status: design / unimplemented
Last revised: 2026-05-10
Tracks audit findings: P0-E1, C-P1-6, C-P1-7 (partial), and a
proposed extension that subsumes the FL r6 P2-2 "instance shape
hint" gap.

This is the complete formal treatment of how `xi::ShmRegion` should
allocate, refcount, transfer ownership of, and reclaim shared blocks
that may carry images **or** opaque byte buffers (ML weights,
metadata, ...) across the host / worker process boundary.

It is a design doc. The actual code change lands in phases tracked
under `.fl_audits/FIX_PROGRESS.md`.

---

## 1. Problem statement

The current `xi::ShmRegion` is a bump-only allocator backed by a
512 MB shared mapping. `release()` decrements refcount but never
returns a block to the pool; `bump_offset` only advances. Under
sustained 30 fps × 1 MB image emit (typical isolation:process
inspection cell), the region exhausts in **~17 s**. Post-exhaustion
behaviour is silent: the worker's `shm_create_image` returns
`INVALID_HANDLE`, the worker silently falls back to a heap handle
that the host's `image_data()` cannot resolve, and the pipeline
output corrupts without alarm.

We want an allocator that:

- Reclaims released blocks so steady-state load is sustainable.
- Preserves the cross-process refcount semantics so existing
  plugin / script code still works.
- Does not introduce a use-after-free race window on the wire
  between worker `release` and host `addref`.
- Gives `O(1)` `alloc` / `release` on the hot path.
- Provisions known-shape outputs at project open instead of
  runtime, so "out of memory" is detectable at boot, not as silent
  degradation in production.
- Still handles variable-output workloads (template matching,
  cascade detection, loop processing) without forcing every plugin
  author to declare worst-case bounds.

## 2. Data model

```
Region    R = [bytes_0, bytes_1, ..., bytes_{N-1}]              (shared mmap)
Block     B = (header, payload),  located at some offset in R
Kind      K(B) ∈ { Image, Buffer }
Handle    H = (tag : u8, offset : u56)                           (identifies B)
Refcount  C : Block -> atomic int32                               (lives inside B.header)
```

Both processes (host, worker) `mmap` the same `R`. All atomic
operations are LOCK-prefixed instructions on the shared bytes —
cross-process equivalent to cross-thread.

Blocks come in two flavours:

| Kind   | Allocator entry | Header carries     | Typical size              |
|--------|-----------------|--------------------|---------------------------|
| Image  | `alloc_image(w,h,ch)` | width, height, channels | tens of KB to a few MB |
| Buffer | `alloc_buffer(size)`  | size only          | KB (metadata) or GB (ML weights) |

`R` is shared between both kinds — the formal model treats them
uniformly as `Block`s with refcount, except where bucket choice
needs to consider both populations (see §10).

## 3. Operations

```
alloc(size, owner_id) -> H        allocate a fresh B; C(B) := 1; tag block with owner_id
addref(H)                         C(B) := C(B) + 1
release(H) -> c'                  c' := C(B) - 1; C(B) := c'
                                  if c' == 0 ∧ allocator.reclaims:
                                      ReturnToPool(B, owner_id)
read(H) -> ptr                    return pointer to B.payload
```

`owner_id` is a per-instance opaque identifier. The allocator uses
it to decide whether `B` came from an instance-private pool (Tier 1)
or the shared pool (Tier 2). Existing plugins that don't carry
`owner_id` get the default "shared" pool (Tier 2) — fully back-compat.

The allocator is parameterised by:

```
reclaims         : bool                          // false for current bump-only
ReturnToPool     : (Block, owner_id) -> Pool     // dispatches to A1 / A2
```

## 4. Invariants

```
I1   live(B, t)  ≡   ∃ t_a ≤ t : alloc returned B at t_a
                  ∧  ∀ t' ∈ [t_a, t] : C(B)(t') > 0
                  ∧  B has not been ReturnToPool'd then re-alloc'd in [t_a, t]

I2   valid_read  ≡   ∀ H, t :
                       read(H, t) returns the bytes the owner wrote
                       ⟺  live(B(H), t)

I3   refcount_health  ≡   ∀ B, t :
                            C(B)(t) ≥ 0
                          ∧ C(B)(t) == |owners holding H(B) at t|

I4   no_double_free   ≡   ∀ B :
                            |alloc events for B|  ==  ∑ over time of release events

I5   no_torn_handoff  ≡   ∀ H crossing a process boundary via send_frame :
                             ∀ t between sender's pre-send and recipient's first-use :
                               C(B(H))(t) ≥ 1
```

`I5` is the new invariant that closes the C-P1-6 race window (§7).
`I1-I4` track standard allocator correctness.

The current bump-only allocator weakens `I1` to "ever allocated
implies forever live" → `I2` is automatic but `R` exhausts → `I1`'s
auxiliary "bounded resource use" reading is violated. Any
`reclaims = true` allocator must establish `I5` to keep `I2`.

## 5. The lifecycle of one image

Worker is producer, host is consumer. `H` corresponds to a single
emitted image:

```
time   actor          action                          C(B) after
─────  ────────────   ─────────────────────────────  ──────────
t₀     worker         H ← alloc(size, owner=src_iso)      1
t₁     worker         memcpy(payload(H), data)            1
t₂     worker         send_frame(H) over IPC              1
t₃     worker         release(H)             ⚠️           0    ← race window opens (current model)
t₄     host (kernel)  ReadFile completes                  0
t₅     host           handle_async_frame_(H)              0
t₆     host           TriggerBus::emit addrefs            1    ← race window closes
t₇     host           dispatcher runs inspect              1
t₈     host           dispatch_end release(H)             0
```

Define the **race window** `W = [t₃, t₆]`.

## 6. Race specification

```
For any allocator and time t ∈ W :

  case reclaims = true :
    ∃ third party thread T_x at time t_x ∈ W :
      T_x.alloc(size') pops B from the free pool
    →  H still decodes to the same offset in R after t_x
    →  read(H, t') for t' > t_x returns whatever T_x wrote
    →  violates I2.

  case reclaims = false :
    B remains at its offset in R, not overwritten.
    →  read(H, t) still returns the bytes from t₁.
    →  I2 holds (but R fills under unbounded execution).
```

Hence the safety / liveness trade-off:

```
bump-only      : safety ✓ ,  liveness ✗   (R exhausts)
naive reclaim  : safety ✗ (in W) ,  liveness ✓
```

To get both, we must either:

(a) eliminate `W` so `C(B)` never transits through `0` while a
    cross-process handoff is in flight  — i.e. establish `I5`; or
(b) prove that during `W` no party can re-alloc `B`  — i.e. weaken
    the allocator's eagerness to reclaim.

This spec adopts (a) via Layer B (§9).

## 7. Layered architecture

The problem decomposes into two orthogonal layers:

```
Layer A (allocator):    free / reclaim policy
  └── A1: instance-private static pool   (fast path, predictable shapes)
  └── A2: shared size-class free-list    (fallback, variable shapes)
  └── A3: bump fallback                   (last resort)
  └── exposes alloc / release / read; agnostic to ownership semantics

Layer B (ownership):    refcount semantics on the wire
  └── ownership-transfer protocol (Route B); see §9
  └── orthogonal to Layer A — works the same regardless of which Tier alloc'd
```

> **Key observation**: Layer A determines whether the race window
> `W` is fatal. Layer B determines whether `W` exists at all.

We pick Layer B such that `W = ∅`. Then any sane Layer A is safe.

## 8. Layer A — tiered allocation

### 8.1 A1 — instance-private static pool (Tier 1)

Plugin manifest declares the **expected shape** of each output:

```json
{
  "name": "edge_detector",
  "manifest": {
    "outputs": [
      {
        "name": "edges",
        "kind": "image",
        "size_hint": { "w": 1920, "h": 1080, "ch": 1 },
        "max_in_flight": 4
      },
      {
        "name": "matches",
        "kind": "image",
        "variable": true                      // opt out of static
      }
    ]
  }
}
```

At project open, for each instance × declared static output, the
allocator pre-reserves `max_in_flight` chunks of the declared
bucket size (rounded up). These chunks belong to the instance's
**private pool**.

```
private_pool : per (instance, output_name) -> LIFO of free Block*

alloc(size, owner) :
  if owner has private_pool[bucket(size)] non-empty :
    return CAS-pop from instance's private_pool
  else fall through to A2

release(H) :
  let B = block_of(H)
  c' := C(B) - 1; C(B) := c'
  if c' == 0 :
    if B was minted from a private_pool :
      CAS-push back to that private_pool
    else fall through to A2's free-list
```

Properties:

- **`O(1)` lock-free hot path**: each instance pops from its own
  LIFO; no cross-instance contention even at the same bucket size.
- **OOM at boot, not runtime**: project open reserves the total
  upfront. Cannot reserve → open_project fails with a clear error
  ("instance X needs 4 × 8 MB but only 16 MB free in region").
  Sysadmin learns about provisioning at boot, not via silent
  pipeline degradation in production.
- **Provisioning visibility**: project.json's instances + their
  manifest hints sum to a known SHM footprint.
- **Hot reload**: on `recompile_project_plugin`, instance pools are
  released back to the shared free pool, then re-reserved post
  reload (mirrors the existing instance dtor / re-create flow).

A1 is **opt-in per output**. Outputs that don't declare `size_hint`,
or set `"variable": true`, fall through to A2.

### 8.2 A2 — shared size-class free-list (Tier 2)

The main reclaiming allocator. Used by A1 misses, by variable-
output plugins, by loop-spawned downstream plugins, and by anyone
not declaring a size hint.

```
buckets        : { 4 KB, 64 KB, 256 KB, 1 MB, 4 MB, 16 MB, 64 MB, 256 MB }
                  ↑     ↑     ↑      ←──── images ────→  ←── ML weights ──→
                  metadata + small buffers

free_head[i]   : atomic<u64>  =  (tag : u16, offset : u48)
                                  ↑ monotonically incremented per push (ABA defense)
                                  ↑ offset of next-free block in bucket i
```

Each free block reuses 8 bytes of its own header pad as
`next_free_offset` — no extra metadata needed in the region.

Operations:

```
A2_alloc(size) :
  i := ceil_to_bucket(size)
  loop :
    snap := free_head[i].load()
    if snap.offset == 0 : break    // free-list empty for this bucket
    B := block_at(snap.offset)
    next := B.next_free_offset
    new_head := (snap.tag + 1, next)
    if free_head[i].CAS(snap, new_head) : return handle_for(B)
  // miss: bump-allocate a bucket-sized block, A3 fallback below

A2_release(B) :
  // assumes C(B) just hit 0
  i := bucket_of(B)
  loop :
    snap := free_head[i].load()
    B.next_free_offset := snap.offset
    new_head := (snap.tag + 1, B.offset)
    if free_head[i].CAS(snap, new_head) : return
```

Properties:

- `O(1)` `alloc` / `release` (one CAS each on success path).
- Lock-free, cross-process safe via 64-bit CAS + ABA tag.
- Internal fragmentation bounded by bucket rounding (≤ 3-6× worst
  case for a single odd request; typically < 1.2× for inspection
  workloads where image sizes cluster at fixed dims).
- No coalescing — fragmentation does not compact. If a workload
  stresses this, swap A2 for a buddy allocator without changing
  Layer B or A1.

### 8.3 A3 — bump fallback (Tier 3)

A2 free-list miss for bucket `i` → bump-allocate a fresh chunk of
bucket size `i`. Same bump pointer as today's `xi::ShmRegion`.

If `bump_offset + chunk_size > N` (region exhausted):

```
overflow_policy ∈ { fail, evict_oldest, log_and_drop }
```

- `fail`: return `INVALID_HANDLE`. Caller's `shm_create_image`
  fails. Worker logs (per existing P0-E1-visibility patch) and the
  promotion-to-SHM step is skipped for this frame.
- `evict_oldest`: walk free pool, pop oldest entry. Risky under
  contention, not recommended for v1.
- `log_and_drop`: same as `fail` but with a louder log. Default
  for v1.

A3 should be rare in steady-state because A2's free-list already
recycled most blocks. It's only reached when the high-water-mark
genuinely needs to grow.

### 8.4 Allocator state diagram

```
                                ┌─────────────┐
                  bucket hit    │  A1 private │  bucket hit
            ┌─────────────────►│   instance  │◄────────────────┐
            │                  │     pool    │                 │
            │                  └──────┬──────┘                 │
            │                         │ miss                   │
            │                         ▼                        │
            │                  ┌─────────────┐                 │
   alloc()  │   bucket hit     │  A2 shared  │    release()    │  if private:
   ────────►│◄────────────────┤  size-class ├────────────────►│  back to A1
            │                  │   free-list │                 │  else: back to A2
            │                  └──────┬──────┘                 │
            │                         │ miss                   │
            │                         ▼                        │
            │                  ┌─────────────┐                 │
            │                  │  A3 bump   │                 │
            └──────────────────┤  fallback  │                 │
                               └─────────────┘                 │
                                                                │
                                                                ▼
                                                         (release back to
                                                         the same Tier
                                                         the block came from)
```

### 8.5 Memory layout: alignment + stride

The allocator pre-commits to two alignment rules that every Tier
honours:

```
ALIGN-1 (header alignment):
   ShmBlockHeader is alignas(64). Header end == payload start, so
   payload start is automatically 64-byte aligned. Covers SSE/AVX/
   AVX-512 load alignment, cache-line locality, GPU page-coalescing.

ALIGN-2 (row stride):
   For Image blocks, stride := align_up(w * channels, kRowAlign)
   where kRowAlign = 64. Each pixel row starts on a 64-byte boundary;
   no row crosses a cache-line boundary mid-traversal. cv::Mat (and
   any SIMD pixel-by-pixel kernel) reads each row as one contiguous
   aligned span.
```

**Block header field budget** (extends §2 data model):

```
struct alignas(64) ShmBlockHeader {
   uint32_t  magic;                  // BLOCK_MAGIC
   uint32_t  kind;                   // 0 Image, 1 Buffer
   int32_t   width, height, channels;
   int32_t   payload_size;           // bucket-rounded
   int32_t   stride;                 // for Image; row_bytes aligned to kRowAlign
   uint8_t   tier_origin;            // 0 = A1, 1 = A2, 2 = A3 (bump fallback)
   uint8_t   _pad_byte[3];
   int32_t   owner_instance_id;      // A1 only; 0 for A2/A3
   uint64_t  next_free_offset;       // free-list link when refcount=0
   std::atomic<int32_t> refcount;
   uint32_t  _reserved[N];           // pad to 64 bytes
};
static_assert(sizeof(ShmBlockHeader) == 64, "");
```

The `tier_origin` + `owner_instance_id` pair is the dispatch key for
`release(H)`: O(1) read from the header decides whether `B` returns
to instance pool A1, shared bucket A2, or — for A3 bump fallback —
just the refcount goes to 0 with no free-list push (A3 has no
recyclable list; the block is leaked-into-bump until project close).

`next_free_offset` is unused while `refcount > 0`. When a block goes
on a free-list, the field stores the offset of the next free block
in the same bucket / pool, forming a singly-linked LIFO. Push and
pop use 64-bit CAS on the head, with ABA defense in the head's top
16 bits (monotonic tag).

**Bucket size implication**: bucket size always equals `align_up(
sizeof(ShmBlockHeader) + max_payload_for_bucket, 64)`. A 256 KB
"payload bucket" actually consumes 256 KB + 64 B per chunk; the
buckets quoted in §10 are payload sizes.

**Worked example**: `alloc_image(640, 480, 1)`:

```
row_bytes = 640 * 1 = 640
stride    = align_up(640, 64) = 640      // already aligned
pixels    = stride * height = 640 * 480 = 307,200 ≈ 300 KB
bucket    = ceil_to_bucket(300 KB) = 1 MB
header    = 64 B aligned + reserves bytes [0..64)
payload   = bytes [64..1,048,640)
data ptr  = base + offset + 64
```

vs `alloc_image(641, 480, 1)`:

```
row_bytes = 641
stride    = align_up(641, 64) = 704      // up by 63 padding bytes
pixels    = 704 * 480 = 337,920
bucket    = 1 MB    // same bucket
```

The stride is reported in `block_header.stride`. Script-side
`xi::Image::stride()` and host-side `image_stride(h)` already exist
(see `xi_image.hpp`); they return this value verbatim.

## 9. Layer B — ownership transfer protocol

Defines the **wire semantics** for cross-process handoffs. Closes
the race window `W` from §6 by maintaining `I5`.

```
INVARIANT (B-OT):
  send_frame(H) carries ownership transfer.
  pre  : sender holds 1 ref to B
  post : recipient holds 1 ref to B,  sender holds 0 refs
  C(B) is unchanged across the IPC operation (1 → 1).
```

Implementation:

```
WORKER (producer):
  H ← alloc(...)                  // C := 1
  fill payload
  send_frame(H)                   // success path: do NOT release
                                  // failure path: release(H) to compensate
                                  //   (recipient never came into existence)

HOST (consumer, in handle_async_frame_):
  parse H from frame
  // Do NOT addref. The wire frame already conferred 1 ref.
  TriggerBus::emit(H, ...)        // bus takes ownership of the inherited ref

HOST (TriggerBus::emit):
  no addref on incoming H         // change vs current code
  if no sink subscribed :         // event will be dropped
    release(H)                    // bus releases the inherited ref
  else :
    enqueue event to dispatcher   // dispatcher will release after inspect

HOST (dispatcher worker):
  pop event
  run xi_inspect_entry            // script reads via t.image()
  release(H)                      // matches the wire-conferred ref → C := 0 → recycle
```

Compensation path: any IPC failure on the sender side (broken pipe,
write failure, peer disconnect mid-frame) requires the sender to
`release(H)` to balance ownership, otherwise the slot leaks.

```
on send_frame failure :
  sender release(H)               // recipient never got the ref

on TriggerBus::emit dropped path :
  bus release(H)                  // no sink will ever release

on worker shutdown with un-sent frames in send queue :
  worker release(H) for each      // recipient won't come

on host crash with in-flight inspects :
  workers are isolated processes; their refs are irrelevant to host's
  view. SHM region is destroyed when host's mapping closes.
  (cross-version: if the worker outlives a host crash and reattaches
  to a new host's region, all refcounts are reset; this is consistent
  with the SHM version handshake from C-P1-1.)
```

### Why Route B over the alternatives

| Route | Wire-time C(B) | Failure handling | Cost |
|-------|----------------|-------------------|------|
| A — refcount-2     | 2 → 2 → 1     | Sender forgets release on drop → leak | needs paired alloc/release; symmetric API change |
| **B — ownership transfer** | **1 → 1**    | Sender release on send-fail; bus release on drop | API change in 3 spots (worker emit, bus emit, dispatcher release) |
| C — two-phase commit | 1 → 1        | Round-trip ack absorbs failures | Extra IPC RTT per emit; latency-prohibitive at 60 fps |

## 10. Bucket sizing — Image-only, project-configurable

### 10.1 Design pivot: SHM only for "memcpy-painful" sizes

Earlier drafts of this spec defaulted to 8 buckets covering 4 KB to
256 MB. Locked decision (2026-05-10): SHM is for blocks where
zero-copy *actually wins* against IPC frame memcpy.

Memcpy bandwidth on modern x86 is ~10-15 GB/s. A 16 MB memcpy is
~1.3 ms — at this size, IPC handoff starts to bite. Below that, the
extra `shm_create_image + memcpy + send_handle` round-trip costs
*more* than just sending the bytes inline in the wire frame.

Default policy:

```
size < promote_threshold  →  IPC frame inline (memcpy via wire)
size ≥ promote_threshold  →  SHM zero-copy (allocator manages)
```

`promote_threshold` defaults to **16 MB** (matches IPC
`MAX_PAYLOAD_BYTES` so a single constant gates both paths).

### 10.2 Default buckets

```
default buckets (project.json `shm.buckets_mb`):
   [16, 64, 256]    // 3 buckets, MB
```

Workload alignment for typical inspection:

```
  image                     bucket   in-flight × instance × cam   region needed
─────────────────────────────────────────────────────────────────────────────────
  5 MP color (15 MB)         16 MB   3 × 1 × 4 cam = 180 MB        512 MB OK
  20 MP color (60 MB)        64 MB   3 × 1 × 4 cam = 720 MB        1 GB recommended
  50 MP color (150 MB)       256 MB  3 × 1 × 4 cam = 1.8 GB        2 GB required
  4K × 3 (25 MB)             64 MB                                 — same bucket as 20 MP
```

Smaller images don't appear here because they don't enter SHM.

### 10.3 Project configuration

`project.json` accepts an optional `shm` block:

```json
{
  "name": "...",
  "shm": {
    "region_size_mb":           1024,
    "promote_threshold_bytes":  16777216,
    "buckets_mb":               [16, 64, 256],
    "max_in_flight_per_instance": 3
  },
  "instances": [...]
}
```

All keys optional; defaults applied per-key when missing. Backend
reads the block at `cmd:open_project`, passes through `ShmConfig`
struct to `ShmRegion::create`, and stores the same values in the
region header so `xinsp-worker.exe` reads consistent values when it
`attach()`s.

### 10.4 Workload presets

| Preset | Use case | `shm` overrides |
|--------|----------|-----------------|
| **Default** | ≤ 5 MP cameras, in-process inspection | (none — defaults work) |
| **Mid-resolution** | 20 MP × 4 cam, future-ready | `region_size_mb: 1024` |
| **High-resolution** | 50 MP × 4 cam, ML inference | `region_size_mb: 2048` |
| **Latency-tight** | High-fps small images, never SHM | `promote_threshold_bytes: 1073741824` (1 GB) — effectively disable SHM; everything goes inline IPC |
| **Memory-tight** | Lots of small instances, prefer wire copy | `region_size_mb: 256` |

### 10.5 Future: bucket auto-tuning

A later phase may add `xinsp-cli shm-tune --project ... --window 60s`
that runs a workload, profiles the SHM allocation distribution, and
emits a recommended `shm` block:

```
$ xinsp-cli shm-tune --project demo --window 60s
Observed alloc histogram:
   60 MB:  88% (cam_left, cam_right at 20 MP color)
   30 MB:  10% (downstream resize result)
    < 1 MB: 2%  (ignored; below threshold)

Recommended shm.buckets_mb: [32, 64]
   Rationale: tighter bucket for 30 MB cluster saves 20% SHM vs default 16/64/256
   Region: 768 MB sufficient
```

Out of scope for v1 — listed here so the data structures don't
preclude it.

### 10.6 Internal fragmentation

With buckets `[16, 64, 256]` MB, worst-case rounding overhead is
≤ 4× (a 17 MB request lands in 64 MB bucket). For the typical case
(~16 MB / ~60 MB / ~150 MB image clusters), rounding is ~5-10%.

If profiling shows a workload where fragmentation matters, project
adds intermediate buckets via `buckets_mb: [16, 32, 64, 128, 256]`.
The runtime accepts up to **8 buckets** in its array (compile-time
cap on `free_head[]` size).

## 11. Variable-output and loop-processing workloads

`max_in_flight` and `size_hint` cover predictable shapes. Variable
outputs (template matching producing 0-N matches per inspect, loop
processing per match, cascade detection) cannot be statically
provisioned without forcing every plugin author to declare worst-case
upper bounds.

Two paths:

- **(default) Tier-2 fallback for variable outputs**.
  The output is declared `"variable": true` (or the field is omitted
  → also treated as variable). Allocations route directly to A2.
  Plugin authors don't need to think about it.
- **(opt-in) Worst-case static provisioning**.
  Plugin manifest declares `max_per_event : N` for variable outputs.
  Project open reserves `N × parent_event_queue_depth × bucket_size`
  in A1. Real-time-friendly; high memory cost. Used for hard
  real-time deployments that need bounded latency on every frame.

xInsp2 default is variant 1 (Tier-2 fallback). The `max_per_event`
field in manifest is parsed and reserved when present, ignored when
absent.

## 12. Worked example — multi-camera with template matching

Project topology:

```
sources:
  cam_left  (isolation:process, image 1920×1080×1)
  cam_right (isolation:process, image 1920×1080×1)

detectors:
  edge_detector_left  (input: cam_left.img)
    output: edges_left (image 1920×1080×1, max_in_flight=4)

  match_finder_left   (input: edges_left)
    output: matches    (variable, 0..100 per event)

inspect script:
  for match in matches:
    apply per-match-fingerprint(match)
```

A1 allocations at project open:

```
cam_left.img                : 4 × bucket(16 MB)  = 64 MB    (camera buffer pool)
cam_right.img               : 4 × bucket(16 MB)  = 64 MB
edge_detector_left.edges    : 4 × bucket(16 MB)  = 64 MB
match_finder_left.matches   : variable, no A1 reservation
                                                 ─────────
                                            total = 192 MB

A2 reserves the rest of the 512 MB for:
  match_finder output blocks (variable count, small KB each → 4 KB / 64 KB buckets)
  per-match fingerprint outputs (loop-spawned, sized at runtime)
  any size_hint miss across the project
                                                 ─────────
                                                  ≈ 320 MB headroom in A2
```

Steady-state hot path, zero contention:

```
Frame N from cam_left :
  worker.cam_left:
    H ← cam_left.private_pool.pop()     // A1 hit, instance-private
    memcpy(... H ...)
    send_frame(H)                       // ownership → host
  host.dispatcher:
    inspect runs:
      H_edges ← edge_detector.private_pool.pop()  // A1 hit
      memcpy edges into H_edges
      ...
      matches ← match_finder.process(...)
        for k in 0..N:
          H_match[k] ← shared_pool[64KB].pop()    // A2 hit
          ...
      release H, H_edges, H_match[*]
        cam_left's H → cam_left.private_pool        // A1 return
        edge_detector's H_edges → edge_detector.private_pool
        H_match[*] → A2 shared bucket-64KB free-list
```

If a 50-match event lands and A2 64-KB bucket only has 30 free →
A3 bump-allocates the extra 20 → those 20 will rejoin A2 on
release (not bump back). Steady-state count grows once, then
recycles forever.

## 13. Correctness argument (sketch)

### 13.1 `I3` (refcount health)

- `alloc` sets `C := 1`.
- `addref` and `release` are atomic increments / decrements.
- Each wire-frame is **ref-conserving** (the `B-OT` invariant of §9).
- Each `release` corresponds to some past `alloc` or `addref` by
  the same logical owner.

By induction over the operation history,
`∀ B, t : 0 ≤ C(B)(t) ≤ |alloc-time refs allotted|`.

### 13.2 `I5` (no torn handoff)

- `B-OT` says `C(B)` does not change across `send_frame`.
- The compensation rules (sender release on send-fail; bus release
  on dropped event) ensure no leak when handoff fails.
- Therefore `C(B) ≥ 1` from `t_a` (worker alloc) until at least
  the first host-side `release` event after successful dispatch.
- `W = ∅`.

### 13.3 `I2` (valid read)

- For `B` to be `ReturnToPool`'d, `C` must transit through `0`.
- `C = 0` implies some `release` event by the last owner.
- That owner cannot subsequently `read(H)` (ownership relinquished).
- By `I3`, no other owner exists at that moment.
- By `I5`, no in-flight handoff still references `B`.
- So no `read(H)` happens while `B` is in the free pool.

### 13.4 Liveness

- Every `B` whose owners eventually all release reaches `C = 0`.
- A1 owns return triggers push to instance pool.
- A2 owns return triggers push to shared bucket.
- Both are bounded-time; future `alloc` requests pop them in `O(1)`.
- Bump pointer only advances when both A1 and A2 miss for the
  bucket. Steady-state load (alloc rate ≈ release rate) stops
  advancing the bump pointer once the high-water-mark is reached.

`R` does not exhaust under any bounded-concurrent-image workload.

## 14. Plugin / SDK API impact

Plugin author surface:

```diff
plugin.json
  "manifest": {
    "outputs": [
-     { "name": "edges", "kind": "image" }
+     { "name": "edges", "kind": "image",
+       "size_hint": {"w": 1920, "h": 1080, "ch": 1},
+       "max_in_flight": 4 }
    ]
  }
```

Backwards compatible: omit the new fields → output goes through
A2 (Tier 2), behaviour matches today's experience minus the silent
exhaustion.

Plugin code surface (worker side, opt-in API revision):

```diff
- xi_image_handle h = host->shm_create_image(w, h, ch);
- ... fill bytes ...
- host->emit_trigger(name, tid, ts, &record, 1);
- host->image_release(h);    // ⚠️ creates the race window
+ // New "loan/publish" API, ownership-transfer aware:
+ xi_image_handle h = host->shm_loan_image(w, h, ch);
+ ... fill bytes ...
+ host->shm_publish(name, tid, ts, &record, 1);
+ // No release. send_frame transfers ownership.
```

Old API stays available for back-compat, but is documented as
"deprecated; prefer loan/publish for new plugins". The wrapper for
the old API does the right thing internally (the `image_release`
becomes a no-op when paired with a successful `emit_trigger`, and
fires on the failure path).

SDK side: no Python SDK change. The wire is all backend-side.

## 15. Test strategy

Properties to verify:

```
P1   no_torn_image        ∀ image emit at t_emit, ∀ host read at t_read > t_emit:
                          read returns exactly the bytes written before t_emit
                          (until inspect releases the ref)

P2   allocator_progress   under steady-state load (alloc / release at fixed rate),
                          R never exhausts; bump_offset stops growing

P3   no_double_release    each H seen on a wire passes through release()
                          exactly once (in aggregate across all owners)

P4   no_use_after_free    no read(H) returns bytes from a block that has been
                          ReturnToPool'd

P5   instance_isolation   under N concurrent emits on the same A1-backed
                          instance, no cross-instance pool contention shows
                          up in flame graphs

P6   provisioning_at_boot project open with insufficient SHM for declared
                          A1 reservations fails open_project, doesn't enter
                          a degraded run-time state
```

Test harnesses:

- **Unit (`backend/tests/test_xi_shm_*.cpp`)**: alloc/release sequences;
  assert `bump_offset` bounded under steady-state; assert refcount
  invariants under simulated handoff.
- **Stress (`examples/fl_r10_shm_soak/`)**: multi-source 60 fps × 10
  min; assert RSS climbs once to peak and stays there; A2 bucket
  high-watermark stable.
- **Race harness**: worker emits image with sentinel pattern,
  PageHeap / Application Verifier enabled, host's dispatcher
  inspects at the moment of worker's would-be release; assert
  script reads sentinel, not garbage. 10 K iters.
- **Provisioning test**: project with output declarations exceeding
  SHM region size → `open_project` fails with explicit
  "insufficient SHM for declared reservations" error.
- **Variable-output test**: template matching plugin emitting
  0-100 matches per inspect; assert all per-match outputs land in
  A2; A2 bucket high-watermark grows once then plateaus.
- **Model checking (optional, future)**: extract the operation
  ordering as TLA+ spec; check `I2` / `I5` invariants under all
  thread/process interleavings up to N steps.

## 16. Implementation phasing

```
Phase 1 (closes P0-E1 + C-P1-6):
   - Implement Layer A2 (shared size-class free-list)
   - Implement Layer B (ownership transfer; loan/publish API + back-compat shim)
   - Validate via fl_r8 full suite + soak test

Phase 2 (closes the remaining provisioning gap):
   - Implement Layer A1 (instance-private pools)
   - Extend plugin.json manifest with size_hint / max_in_flight
   - Project open reserves A1 chunks; fails fast on insufficient SHM
   - Validate via provisioning test + steady-state hot-path benchmark

Phase 3 (optional, real-time hardening):
   - Implement max_per_event for variable outputs
   - Add overflow_policy.fail vs log_and_drop choice in project.json
   - Validate against bounded-latency requirement under bursty load
```

Phase 1 is the blocker for production deployment; Phases 2-3 are
performance / provisioning improvements layered on top.

## 17. Prior art

| System            | Idea borrowed                                       | Adoption |
|-------------------|------------------------------------------------------|----------|
| Eclipse iceoryx   | Loan/publish API → ownership transfer enforced by API shape | **Steal API naming**: `shm_loan_image` / `shm_publish` |
| Eclipse iceoryx   | `MemPool` size-class CAS-based free-list             | **Read implementation as reference**: `iceoryx_hoofs/source/posix_wrapper/mempool.cpp` (~300 lines, Apache 2.0) |
| Eclipse iceoryx   | RouDi central broker daemon                          | Not adopted — architectural mismatch (xInsp2's host IS the broker) |
| GstBufferPool     | acquire/release-back-to-pool semantics on refcount=0 | Validates Layer A2 design — already convergent |
| GStreamer caps    | Format negotiation between elements                  | Not adopted — instance.json + plugin.json manifest already cover this differently |
| GstMemory         | map/unmap with access flags                          | Optional Layer B+ enhancement: enforce unmap-before-reclaim via RAII guard |
| Boost.Interprocess| `managed_shared_memory` with multiple algorithms     | Not adopted — heavy dep, our surface is small |
| TLSF              | Real-time O(1) allocator with bounded fragmentation  | Considered as alt for A2; size-class chosen for simpler cross-process semantics |
| Apache Plasma     | Cross-process tensor object store                    | Not adopted — daemon-based, architectural cost |

## 18. Out of scope

- **Cross-version SHM layout drift** — already addressed by the
  version handshake in PR #50 (audit C-P1-1).
- **Buddy allocator with coalescing** — listed as a swap-in
  alternative for A2 if size-class fragmentation becomes a problem
  in practice. Same correctness argument applies; only `alloc` /
  `ReturnToPool` change. Not implemented in v1.
- **Cross-process garbage collector** ("which refs belong to a
  dead pid?") — if a process crashes holding refs, those refs
  leak. The audit notes this in the existing bump-only world; it
  does not change under this spec. A separate design.
- **Backing storage swap** (heap ↔ SHM ↔ GPU) — GstAllocator
  pluggability is a future generalisation. xInsp2 v1 keeps SHM as
  the single backing.
- **Hot-replanning A1 sizes** while the project is running —
  re-provisioning on `recompile_project_plugin` is supported (the
  instance dies and respawns). Live re-tuning of `max_in_flight`
  without an instance restart is not in v1.
- **Distributed deployment** (host on machine A, workers on
  machine B) — out of scope. SHM is intra-host.

## 19. Open questions for the implementer

1. **Bucket boundaries**: are the 8 sizes from §10 final, or should
   they be project-configurable (in `project.json` parallelism block)?
2. **A1 reservation strategy**: contiguous within R, or scatter-
   gather? Contiguous gives better cache locality on hot path but
   complicates A1↔A2 boundary tracking.
3. **A3 overflow_policy default**: `fail` is safest but breaks
   workloads transient over the bump cap. `log_and_drop` continues
   the run with degraded quality. Which is the project default?
4. **Loan API naming**: `shm_loan_image` / `shm_publish` matches
   iceoryx; `shm_acquire_image` / `shm_emit_with_handle` matches
   GstBufferPool. Style choice; consistency across the host_api
   matters more than which.
5. **`max_per_event`** — opt-in or required for `"variable": true`
   outputs in safety-critical deployments? Defaults to none in v1.

---

## Appendix A. Memory model assumptions

This spec assumes:

- **Win32 + x86-64**. CPU is TSO (Total Store Order); LOCK-prefixed
  atomics are full barriers; cache coherency is hardware-enforced
  via MESI. No software fences needed for the refcount /
  free-list-head atomics.
- **IPC is `WriteFile` / `ReadFile` over a Win32 named pipe**.
  Both sides are syscalls and act as compiler + CPU barriers; a
  `memcpy` followed by `WriteFile` cannot be reordered. Receiver's
  `ReadFile` similarly synchronises before any subsequent payload
  read.

Together, these mean **no explicit `std::atomic_thread_fence`
calls are required in the worker→host SHM handoff path**. The
existing LOCK-prefixed atomics on `refcount` and `free_head[]`
provide all the ordering needed.

### When this assumption breaks

- **Linux / POSIX port**: same x86-64 TSO holds; same IPC syscall
  guarantee holds for socket / pipe transfers. Spec applies as-is.
- **ARM port** (e.g. Apple Silicon, AWS Graviton): weaker memory
  model. Audit every `memcpy` paired with an atomic `store-release`
  / `load-acquire`. If we ever switch from syscall IPC to a
  lock-free SHM ringbuffer for control-plane traffic, **this
  appendix needs revision** — the implicit syscall barrier goes away.
- **Distributed deployment** (host on machine A, workers on machine
  B): out of scope; SHM is intra-host.

The free-list CAS already uses `memory_order_acq_rel` semantics
implicit in `std::atomic::compare_exchange_weak`; adopting ARM
without changes there should remain correct.

---

## Appendix B. Telemetry / metrics

The allocator publishes atomic counters in a dedicated cache-line-
aligned region inside `ShmRegionHeader`:

```cpp
struct alignas(64) ShmMetrics {
    std::atomic<uint64_t> a1_acquire_total;       // Tier 1 hits
    std::atomic<uint64_t> a2_acquire_total;       // Tier 2 hits (free-list pop)
    std::atomic<uint64_t> a2_bump_inflate_total;  // Tier 2 bucket bump (free-list miss → bump as bucket-sized)
    std::atomic<uint64_t> a3_bump_total;          // Tier 3 raw bump fallback
    std::atomic<uint64_t> alloc_failed_total;     // Region full → INVALID
    std::atomic<int32_t>  a2_free_count[N_BUCKETS_MAX];  // per-bucket current free count
    // per-instance A1 counters live in their own InstancePoolEntry
};
```

Backend exposes `cmd:shm_metrics` returning the snapshot:

```json
{ "type": "rsp", "id": 42, "ok": true, "data": {
    "a1_acquire_total":      188_447,
    "a2_acquire_total":      11_823,
    "a2_bump_inflate_total": 480,
    "a3_bump_total":         0,
    "alloc_failed_total":    0,
    "a2_free_count":         [12, 4, 0],
    "buckets_mb":            [16, 64, 256],
    "instances": [
      { "name": "cam_left",  "a1_in_use": 2, "a1_pool_size": 3 },
      { "name": "cam_right", "a1_in_use": 3, "a1_pool_size": 3 }
    ]
  } }
```

Suggested production alarm rules:

| Condition | Severity |
|-----------|----------|
| `a3_bump_total > 0` | warn — A2 falling through to bump fallback |
| `a2_free_count[i] / total_count[i] < 0.2` for any bucket | warn — bucket nearly exhausted |
| `alloc_failed_total > 0` | **fatal** — region full; immediate investigation |
| Per-instance `a1_in_use == a1_pool_size` for > 5 s | warn — A1 backpressure |

Counters are monotonic (total counters) or current (free_count,
in_use); never decrement past what makes physical sense. They
survive worker respawn (live in SHM region header, not per-process).

---

## Appendix C. Optional canaries (debug build only)

For dev / canary deployments, compile-time flag
`XINSP2_SHM_CANARY` enables block boundary canaries:

```cpp
struct alignas(64) ShmBlockHeader {
    // ... regular fields ...
    uint32_t header_canary;   // BLOCK_MAGIC_HEADER_CANARY
};

// Tail canary: last 4 bytes of each block's allocated chunk
// Magic: BLOCK_MAGIC_TAIL_CANARY
```

Every `addref` / `release` / `read` validates both canaries; on
mismatch, fatal abort with diagnostic dump (offset, refcount, owner,
neighbour blocks).

### Why off in production

- Canary check on every read costs measurable CPU at 60 fps × 1 MP
  rates.
- Threat model: workers are *trusted* (our own `xinsp-worker.exe`
  + project-local plugin DLLs). Adversarial threat is "buggy plugin",
  not "malicious worker". Canaries catch buggy plugins during dev,
  not after deployment.

### When to enable

- During plugin author dev cycles (set `XINSP2_SHM_CANARY=1` in
  build config).
- "Production canary" build for a small fraction of deployments
  to detect memory-corruption regressions before they go fleet-wide.
- After any P0 audit finding involving SHM corruption.

The canary fields are present in the BlockHeader layout regardless
of build mode (so SHM_VERSION doesn't change), but the validation
code is `#ifdef XINSP2_SHM_CANARY`. Release build pays no runtime
cost; the 8 bytes are unused space.
