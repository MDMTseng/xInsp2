# Roadmap — memory domains (GPU/device buffers)

Status: DESIGN PARKED (2026-07-15, CT + review). Not scheduled. This page
exists so nothing lands that would contradict it.

## The gap

The pool is host-memory only. Deep-learning-era pipelines want
`camera → GPU → infer → post` with zero host round-trips. Cross-plugin
zero-copy is a pool-ABI promise, so device buffers cannot be composed purely
out-of-tree — but almost all of it still belongs in a lib plugin.

## The agreed cut

- **`xi.gpu.pool` cap lib plugin owns ~90%**: device allocation (size-class
  magazines, the pixpool design transplanted), `upload`/`download` verbs,
  every CUDA/D3D/CL detail. GPU-aware plugins resolve the cap and share
  opaque ids; the core never learns a GPU API exists.
- **Pack carries a device blob as a proxy**: a normal blob whose descriptor
  says `{"loc":"cuda:0","gpu_id":…, …}` with an empty/tiny payload. Wire and
  record boundaries materialize via `download` (they copy anyway).
- **The ONE core primitive owed: a pool-handle finalizer.** Pack
  retain/release drives POOL handle refcounts; a device buffer would need
  manual paired release = convention-not-structure = the UAF class we spent
  rounds eliminating. A registered finalizer on the (proxy) handle — invoked
  once at refcount zero — extends pack RAII to any foreign resource (~20
  lines, generic: also future mmap/shm/remote handles).

## Guards already in place (the only work done now)

- Descriptor key `"loc"` is RESERVED for this design.
- `read_image_blob` (and everything sugar-level above it) REJECTS a
  descriptor carrying a non-host `"loc"` — host-only consumers fail loud on a
  device proxy instead of silently reading an empty payload.
- Do not add code that assumes `get_blob().payload` is always the real data.

## Trigger to unpark

The first real GPU consumer (e.g. an inference toolbox). Then: finalizer
primitive first (with tests), lib plugin second, `xi/image`-style `loc`-aware
accessor sugar last.
