# BufferPool Audit — Can ImagePool Serve as the v3 Typeless Large-Buffer Pool?

> [2026-07-14] Container storage superseded by the v3 slab (packv3 branch) — the `pack_pool` facade verdict below still stands (EXTERN entries ride ImagePool unchanged); the arena/container details are historical. See docs/internals/pack-plane.md.

| Field | Value |
|---|---|
| **Date** | 2026-07-02 |
| **Status** | Audit verdict (polaris2 wave-1 task 1e). Companion to [`07-uniform-keyed-buffer-plane.md`](./07-uniform-keyed-buffer-plane.md) and the Frame container prototype (`backend/include/xi/xi_frame.hpp`) |
| **Question** | Can `backend/include/xi/xi_image_pool.hpp` back the v3 *typeless* large-buffer plane as-is, or does it need surgery? |
| **Verdict** | **Yes — reuse as-is behind a thin (<100-line) typeless facade.** No change to `xi_image_pool.hpp`. The image-specific bits are a narrow, dodgeable rim, not load-bearing structure |

## One-paragraph verdict

The pool's core — a lock-free, generation-stamped, refcounted slot array with
an owner ledger, deferred-reclamation slot walks, and the ABA/exhaustion/
overflow hardening earned across reviews 02/08 — is entirely payload-agnostic
and is exactly the "one pool, one ownership discipline, one crash story" doc 07
asks for. The only image-specific surface is three integer dimensions on
`PoolEntry` (`width`/`height`/`channels`) and the handful of accessors over
them; nothing in allocation, refcounting, ownership sweep, or reclamation reads
those dimensions. A typeless buffer is therefore just a `create(n, 1, 1)` entry
whose `n` bytes *are* the payload, resolved back through `read_data` + a
`width*height*channels` size — which is what the Frame prototype's `frame_pool`
facade does today in ~40 lines. Reuse is safe; the facade is optional sugar,
not a blocker.

## What is genuinely image-specific (the rim)

1. **Dimensions in the entry.** `PoolEntry` carries `int32_t width, height,
   channels` and `create(w, h, ch)` allocates `w*h*ch` bytes
   (`xi_image_pool.hpp:68-81`, `:115-170`). This is the *only* domain knowledge
   in the pool. For a typeless buffer the facade passes `(n, 1, 1)`, so the
   byte count is exact and the two spare dims are inert.
2. **Image-named accessors.** `width()/height()/channels()/stride()`,
   `from_image()/to_image()`, and the `xi.imaging@1`/`imaging_rw@1` carved
   interfaces speak `xi::Image`. These are *additive* — a typeless consumer
   simply never calls them; it uses `read_data()` + its own length.
3. **The 1 GiB per-buffer cap and INT32 dimension domain**
   (`xi_image_pool.hpp:130`). `create` rejects `w<=0` and `pixels > 1 GiB`.
   For a typeless `(n,1,1)` buffer this caps a single buffer at `INT32_MAX`
   bytes and, effectively, 1 GiB. Fine for images and every current CV payload;
   see "the one real constraint" below.

## What is already typeless (the load-bearing core — untouched)

- **Handle layout & generation/ABA defense** (`:11-30`, `:101-111`): pure
  `{slot, generation}`, no payload assumptions.
- **Refcount lifecycle** — `create`/`addref`/`release`/last-ref reclaim
  (`:172-196`): counts refs, not pixels.
- **Owner ledger + sweep** — `ImagePoolOwnerId`, `release_all_for`,
  `ImagePoolOwnerScope` (`:256-341`, `:904-930`): the per-producer drop-on-death
  discipline doc 07 wants for its "handles released by the one owner" story.
  Payload-blind.
- **Deferred-reclamation slot walks** — `WalkGuard`/`retired_`/`stats`
  (`:378-400`, `:780-846`): the review-08 UAF fix; the balance oracle the Frame
  tests already lean on (`cumulative().live_now`). Counts entries and
  `pixels.size()` bytes — both meaningful for typeless buffers unchanged.
- **Exhaustion / overflow / bad-alloc hardening** (`:123-151`, `:848-873`):
  all size-and-slot logic, no image logic.

## What a typeless facade needs (and already has, in the prototype)

The Frame prototype implements the whole facade in `frame_pool` inside
`xi_frame.hpp` — no new file, ~40 lines:

- `alloc_bytes(src, n)` → `create(n,1,1)` + memcpy; mint path for large bins.
- `alloc_image(w,h,c,px)` → native `create(w,h,c)` + memcpy; images stay
  first-class (their dims are real, not `(n,1,1)`).
- `view(h)` → `read_data(h)` + `width*height*channels` as the byte length —
  one span for both typeless and image buffers.
- `addref/release` → guarded by `g_image_pool_alive` so a frame destroyed
  during static teardown never touches the dead singleton.

Because both a typeless buffer and an image resolve through the *same*
`view()`, the Frame's `get_bin` (pooled branch) and `get_image` read the
identical pool bytes with no per-kind pool code. That is the "one pool, storage
duality, API unity" of doc 07 D1, realized without touching the pool.

## The one real constraint (not a blocker, a note)

`create` takes `int32_t` dims and caps a buffer at 1 GiB, so the typeless
facade inherits a **~1 GiB / INT32-bytes ceiling per buffer**. This is above
every current CV/tensor payload and matches the image path's existing limit, so
it blocks nothing today. If v3 ever needs a single >1 GiB buffer (e.g. a large
volumetric tensor), the fix is a *pool-side* widening of the dimension/cap
domain to `int64`/`size_t` — a change to `xi_image_pool.hpp` that the facade
would pass straight through. Out of scope here; flagged so it is a known,
localized future edit rather than a surprise.

## Recommendation

Adopt `ImagePool` as the v3 large-buffer pool unchanged. Keep the typeless
facade as a thin, frame-layer-owned shim (as prototyped in `frame_pool`), so
handle minting stays the privileged ext path doc 07's ingress rule requires.
Revisit only the int64 dimension widening, and only if a real >1 GiB single
buffer appears. Do **not** fork the pool or add a parallel typeless pool: that
would re-create the two-registry split (ImagePool + DocRegistry) doc 07 exists
to erase.
