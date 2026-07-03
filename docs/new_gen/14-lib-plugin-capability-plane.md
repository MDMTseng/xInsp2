# 14 — Lib plugins: the host-forwarded capability plane

Status: **design note, unscheduled** (maintainer-settled direction, 2026-07-03).
The provider-registration ABI slot rides THE CUT (v12) train; nothing here is
committed code yet.

## Problem

Some capabilities are inherently **shared across instances** — the motivating
example is de-duplicated JPEG compression: many instances (expose, record_save,
anything that ships an image out) want the same sealed pack's image encoded
once, not once per consumer. A per-DLL cache cannot deduplicate (per-DLL
singletons — the PackRegistry lesson), so the capability and its memo cache
must live in exactly one place that every instance can reach.

Instances cannot call each other (a load-bearing invariant — no ABI door
exists, `xi::use()` is script-side only, and the crash-quarantine model assumes
all calls enter through one host funnel). So where does a shared capability
live?

## Design

**A lib plugin is a capability provider, not a callable instance.**

- A lib plugin is a normal plugin DLL with **no data plane** (it never emits,
  nothing routes to it). On load it registers capabilities with the host:
  `register_capability("xi.jpeg", 1, <pack door>)` — the one new ABI slot,
  v12 train.
- Consumers never see the provider or its vtable. They resolve **by capability
  name** through the host and call through the **host forwarding funnel** —
  the same discipline (and largely the same code path) as the P2
  `use_pack_process_cb` door: Pack in, Pack out, item-14 gates around the call.
- The host owns a **capability registry** (capability name → providing
  instance). Which instance provides a capability is configuration (def), so a
  provider can be swapped without touching any consumer.

### Why host-forwarded (not a direct provider vtable)

1. **Lifetime** — consumers hold a host-owned stub, never a pointer into the
   provider DLL. Provider unload/reload = host re-points the target; no pin
   protocol, no dangling function pointers.
2. **Crash attribution** — SEH wraps the forwarding point; a fault is charged
   to the lib instance and runs *its* on_fault policy. Consumers see the
   established -2 (crashed) / -3 (quarantined) semantics.
3. **Hot swap** — def-driven provider replacement is a host-side re-point.
4. **Observability** — every capability call crosses one funnel: call counts
   and latency land in dispatch_stats for free.

### The one new invariant: reentrancy guard

Host-forwarded capability calls create plugin→plugin call chains (A's
`process()` calls the jpeg lib's door). The host enforces acyclicity at the
forwarding point: a thread-local stack of instances currently being called;
forwarding into an instance already on the current thread's stack is refused
with a distinct code (**-5 reentrancy**). Deadlock and recursion die at the
door. Providers must be thread-safe (multiple dispatch threads call
concurrently) — an ABI-contract requirement, same as the pack vtable.

## Sizing doctrine (maintainer ruling)

The capability plane is sized for **heavy work only** — operations that
comfortably amortize one Pack round-trip plus a forwarding hop (JPEG encode,
model inference, expensive lookups).

- **Too light to amortize the hop ⇒ it is not inter-instance work.** Inline it
  or link it statically. Do not design a "fast path" variant of this plane for
  small calls; if the hop dominates, the boundary was drawn wrong.
- **Private side-channels are off-pattern.** An instance that hand-rolls its
  own cross-instance communication (shared memory, sockets, files, globals)
  steps outside everything the host guarantees: no quarantine boundary, no
  crash attribution, no lifecycle coordination (commit_group quiesce cannot
  see it), no observability. It is explicitly unsupported — not forbidden by
  mechanism, but you are on your own, and nothing in the core will be bent to
  accommodate it.

## Official lib plugin roster (maintainer ruling 2026-07-03: evict what
## doesn't need to live in the host)

Surveyed against the three criteria (heavy, cross-instance shared, host-gated):

**First official lib plugin: `xi.imgcodec`** — image encode + decode + dedup
cache, and it **absorbs `host_api->read_image_file`**. The stb_image reader
(PNG/JPEG/BMP/TGA/GIF/PSD/HDR/PIC) is the one field in today's host_api that
is library functionality rather than core infrastructure — under the
minimal-core philosophy it never belonged there. Moving it out at v12 makes
the capability-plane ABI bill **net zero**: +1 `register_capability`,
−1 `read_image_file`. Encode side (turbojpeg is already deployed beside the
backend) serves expose preview, record_save export, and any comms plugin —
with the sealed-pack identity as the memo-cache key, one encode serves all
consumers. `xi.imgcodec` also proves out the whole plane in-tree (register,
forward, reentrancy guard, quarantine, hot swap) with no external consumer
needed.

**Named-but-deferred (build when the need arrives):**
- `xi.infer` — ML model hosting: one load, N detector consumers, quarantine
  isolates a model crash; owns the RAM/VRAM budget.
- `xi.compress` — zstd/lz4 for record_save archives / large blob export;
  shared dictionaries.
- `xi.calib` — shared calibration/rectification tables (compute-once,
  many-readers, immutable — same shape as sealed packs). Only once a second
  consumer beyond synced_stereo exists; until then instance-folder data.
- `xi.kv` — cross-instance blackboard. RED-FLAGGED: needs semantics decisions
  (versioning vs last-write-wins) and is a hidden-coupling factory by nature;
  revisit only against a concrete need.

**Explicit non-candidates** (the sizing doctrine says no): per-pixel/CV
primitives (plugins statically link OpenCV/IPP — hot path, inline), pack
building / msgpack encoding (header-only SDK, zero-cost), log/status/health/
instance-folder (host core proper).

**Eviction principle (general):** the capability plane is also the host's
slimming exit. Anything in host_api that is a *library* (does work on data)
rather than *infrastructure* (owns identity, lifetime, routing, or safety)
is a candidate to move behind a capability at an authorized break.

## ABI bill

- **Consumer side: zero new slots.** `get_interface("xi.cap", 1)` returns a
  host-owned capability-call vtable (resolve + call by capability name).
- **Provider side: one new slot** (`register_capability`), rides the v12
  authorized break with the rest of THE CUT.
- **Net zero at v12** with the `read_image_file` eviction (see roster).

## Relation to existing planes

| Plane | Caller | Callee | Transport |
|---|---|---|---|
| Data plane | dispatch | plugin door | emit → TriggerBus → process (one-way) |
| Script orchestration | script | plugin door | `xi::use()` host funnel (P2) |
| Control plane | WS client | host lifecycle | commands (create/def/commit, quiesced) |
| **Capability plane (this doc)** | **plugin** | **lib plugin door** | **host forwarding funnel + capability registry** |

One funnel discipline everywhere: every cross-boundary call enters through a
host-mediated door with fault gates. No instance ever holds another instance's
code pointer.
