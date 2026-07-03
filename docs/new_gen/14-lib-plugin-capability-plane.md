# 14 — Lib plugins: the host-forwarded capability plane

Status: **pilot LANDED on the polaris2 line** (pre-v12, 2026-07-03; design
maintainer-settled 2026-07-03). The plane is live code: host registry + funnel
+ reentrancy guard (`xi_cap_abi.hpp` / `xi_cap_guard.hpp`), the first official
lib plugin `plugins/imgcodec`, ctest `cap_plane_test` + `cap_imgcodec_test`,
and QA `examples/qa_cap_imgcodec` — all green. Zero ABI slots were spent (see
the ABI bill below); v12 may still formalize the surface, the pilot proves the
shape. See "The pilot implementation" at the end.

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
  nothing routes to it). On create it registers capabilities with the host
  through a host-owned registration vtable resolved via
  `get_interface("xi.cap.provider", 1)` — **no new ABI slot** (see the ABI
  bill): `register_capability("xi.jpeg.encode", <pack-door-shaped handler>,
  self)`. The registry is **name-only**; capability versioning rides *inside*
  the request pack (the `$v` / `$probe` convention below).
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
the capability-plane ABI bill **net-negative**: −1 `read_image_file`, +0 (the
pilot's zero-slot get_interface route — see the ABI bill). Encode side serves
expose preview, record_save export, and any comms plugin — with the image's
content identity as the memo-cache key, one encode serves all consumers.
(Encoder reality check, corrected by the pilot survey: turbojpeg is NOT
actually deployed beside the backend — `XINSP2_HAS_TURBOJPEG` is an opt-in
cmake switch expecting an external install; the pilot ships the vendored
stb_image_write path, and swapping the encoder later is invisible behind the
capability.) `xi.imgcodec` also proves out the whole plane in-tree (register,
forward, reentrancy guard, quarantine, owner-swept lifecycle) with no external
consumer needed.

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

## ABI bill (pilot refinement: ZERO slots, both directions)

The original bill budgeted one new `xi_host_api` slot for provider
registration on the v12 train. The pilot improves on that: **both directions
ride the existing `get_interface` mechanism** — no `xi_host_api` layout change
at all, so the plane landed pre-v12 without an authorized break.

- **Consumer side: zero slots.** `get_interface("xi.cap", 1)` →
  `xi_cap_v1 { call(name, pack in, pack* out), available(name) }` — the
  host-owned call vtable entering the forwarding funnel.
- **Provider side: zero slots.** `get_interface("xi.cap.provider", 1)` →
  `xi_cap_provider_v1 { register_capability(name, handler, self),
  unregister_capability(name, self) }` — a host-owned registration vtable.
  The registering *instance identity* costs no parameter either: every plugin
  entry point (factory included) already runs under the host's thread-local
  owner context, and registration is attributed through it — the same identity
  the image/pack owner sweeps use.
- **Names carry no version.** The registry is name-only; semantic versioning
  is in-band (`$v`), so the *transport* vtables are the only frozen surface —
  a capability's evolution never touches the ABI.
- **v12 formalization (optional):** THE CUT may still promote the two
  interfaces to first-class documented ABI (they are already frozen per the
  interface doctrine) and pay off the roster's eviction: with
  `read_image_file` removed the bill goes **net-negative** (−1 slot).

### In-band versioning: the `$v` / `$probe` convention

`get_interface`'s version parameter versions the *vtable*, not the
capabilities. A request pack may carry:

- `$v` (i64) — the semantic version the consumer speaks. The provider
  dispatches internally; an **absent `$v` means the provider's documented
  default**; an unsupported `$v` is answered with a normal sealed `$fault`
  pack (`unsupported_version` + a `$versions` entry naming the supported
  range) — the funnel rc stays 0, versioning is capability contract, not
  transport verdict.
- `$probe: true` (bool) — a version/feature probe: the provider answers
  `$versions` (e.g. `"1"`) and does **no work**. Consumers probe once at init
  instead of guessing.

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

## The pilot implementation (polaris2 line, pre-v12)

**Host side** — `backend/include/xi/xi_cap_abi.hpp` (the `xi_pack_abi.hpp`
sibling) + `xi_cap_guard.hpp` (leaf thread-local state):

- `CapRegistry`: name → `{handler, self, owner}`. Registration is legal only
  from **lifecycle code** (factory / set_def / prepare / commit / exchange /
  destroy — enforced: owner context present AND not inside a data-plane door
  or capability handler → else `XI_CAP_REG_ECONTEXT`). A name held by another
  live instance refuses (`XI_CAP_REG_ETAKEN`); same-owner re-registration
  overwrites (the reinit path). Owner sweeps mirror the image/pack discipline:
  adapter dtor, failed-factory scope, and `reinit()` (BEFORE the rebuild
  factory runs — the registered `self` belongs to the corrupted instance being
  replaced; the fresh factory re-registers).
- `cap_call_funnel` (`xi_cap_v1.call`): mirrors `use_pack_process_cb`'s
  discipline in new code — unknown name **-1**; quarantined **-3** (fail-fast,
  plugin not entered; pending reinit applied first); SEH wrap with the fault
  **charged to the lib instance** (its on_fault policy: reuse / reinit /
  refuse→quarantine + health overlay; service adds crash-loop bookkeeping via
  installed hooks) **-2**; unusable entry **-4**; reentrancy **-5** — a
  thread-local stack of instances in host-funnel call chains, checked in BOTH
  directions (consumer calling its own capability; handler calling back up its
  chain) BEFORE any lock, so the CallScope deadlock dies at the door. The
  handler runs under `OwnerGuard(provider)` (allocations attributed to the
  lib) with NO CallScope — providers contract to be thread-safe.
- Published by `install_cap_plane()` next to `install_pack_abi()`
  (default_host_api, certify, tests) through ImagePool slot bridges.

**The first lib plugin** — `plugins/imgcodec` (`"lib": true` marker in
plugin.json, informational): registers `xi.jpeg.encode` (image + quality →
JPEG; content-keyed dedup memo cache — pool-handle identity is not exposed
across the ABI pre-v12, so FNV-1a content hash + params is the key, same
identity the host xi.preview cache uses) and `xi.image.decode` (bytes → image;
stb_image format set — `read_image_file`'s designated v12 eviction target).
`on_fault: refuse`. Counters (`encodes`/`hits`/`decodes`) via exchange
`stats` — the dedup proof instrument.

**Proof** (all green): backend ctest `cap_plane_test` (round-trip, $v/$probe,
-1, crash → -2 + quarantine, -3 fail-fast, -5 both directions, lifecycle-only
registration, ETAKEN, targeted unregister, dtor sweep, pack balance); plugins
ctest `cap_imgcodec_test` (two consumers + one sealed image = ONE encode,
byte-identical JPEG, decode round trip, $fault contract); QA
`examples/qa_cap_imgcodec` (live service: script → consumer pack door →
host-forwarded capability, encode counter pinned at 1 across every run).

**v12 must revisit**: promote/retire the two interfaces formally; pay the
`read_image_file` eviction; expose a stable sealed-image identity across the
ABI (content-hash keying is correct but pays a per-call hash over pixels);
per-capability call counts/latency in dispatch_stats (the pilot funnel does
not meter yet); whether `xi_cap_v1.call` should carry a consumer-declared
timeout.
