# 14 — Lib plugins: the host-forwarded capability plane

Status: **pilot LANDED on the polaris2 line** (pre-v12, 2026-07-03; design
maintainer-settled 2026-07-03). The plane is live code: host registry + funnel
+ reentrancy guard (`xi_cap_abi.hpp` / `xi_cap_guard.hpp`), the first official
lib plugin `plugins/imgcodec`, ctest `cap_plane_test` + `cap_imgcodec_test`,
and QA `examples/qa_cap_imgcodec` — all green. Zero ABI slots were spent (see
the ABI bill below); v12 may still formalize the surface, the pilot proves the
shape. See "The pilot implementation" at the end. **V3 (machine-level autoload —
a provider available with NO per-project instance) also LANDED pre-v12** as a
default-OFF deployment opt-in (`--autoload-lib`); see "V3: machine-level
autoload" below.

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
cmake switch expecting an external install; the pilot shipped the vendored
stb_image_write path, and swapping the encoder later is invisible behind the
capability.) **The encoder swap has now LANDED (polaris2/v12-encoder-eviction):
`xi.imgcodec` encodes through `xi::encode_jpeg` behind the SAME
`XINSP2_HAS_TURBOJPEG` opt-in the backend used, so the SIMD (turbojpeg) path —
and its external dep — MOVE OUT of core and into this lib plugin. The host's
`compress_sink` delegates preview encode to `xi.jpeg.encode` (per-call re-check,
reentrancy/quarantine/$fault → in-core fallback, dedup deferred to imgcodec's
content cache — no host double-cache), byte-identical engine-for-engine (proven:
`cap_jpeg_encode_host_test`). At v12 core's in-core encoder + BACKEND
`XINSP2_HAS_TURBOJPEG` are deleted and `xi.imgcodec` OWNS the SIMD encoder as the
sole encode engine (doc 06 §1 row 9 / doc 10 cut-list).**
`xi.imgcodec` also proves out the whole plane in-tree (register,
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
  revisit only against a concrete need. NAMING NOTE (2026-07-03): the `kv`
  name is since taken by the SCRIPT-LOCAL state store `xi::kv()` (U2, doc 16)
  — an unrelated plane (script SDK, never crosses the ABI). If this
  blackboard is ever revived, pick a different door name (e.g.
  `xi.blackboard`) to avoid teaching two `kv`s.

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
  > **rc-namespace note (cross-ref doc 17):** host-funnel return codes are
  > PER-VTABLE namespaces, not one global enum. `-5` means **reentrancy** on
  > `xi.cap.call` (this table) and **sink-target rejection** on the use-door
  > funnel (`use_pack_process_cb`, doc 17). The two tables are independent
  > and each is frozen in its own doc — never cross-read a code from one
  > funnel against the other's table.
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
byte-identical JPEG, decode round trip, $fault contract); plugins ctest
`cap_imgcodec_host_test` — the **core-codec eviction landed pre-v12**: the
host's `read_image_file` now delegates decode to `xi.image.decode` (per-call
availability re-probe, never caching absence) with a silent built-in-stb
fallback, proven byte-equivalent across both engines (3-channel + native
2-channel gray+alpha via the `raw` reply), factory-timing / absent-capability
fallback, self-serve reentrancy refusal (the decoder is never served by
itself — funnel −5 → stb; its decode counter does not move), decoder-fault
(quarantine −3) clean fallback with the fault still attributed to the lib
instance, and identical fail modes on corrupt/null; QA
`examples/qa_cap_imgcodec` (live service: script → consumer pack door →
host-forwarded capability, encode counter pinned at 1 across every run).

### V3: machine-level autoload (LANDED pre-v12)

The pilot registered a capability only once a PROJECT created a provider
instance (the factory is what calls `register_capability`), so E1's second cause
(doc 06 §6): a deployment with the plane installed but no `imgcodec` instance
still `available("xi.image.decode")==0` → falls back. **V3 removes the
per-project instance requirement.** A provider plugin marks itself
`"autoload": true` in plugin.json (imgcodec does — informational alongside
`"lib"`), and the host instantiates every eligible provider ONCE at service boot
under a stable **machine owner** (`PluginManager::machine_instances_`, keyed by
plugin name, synthetic instance `@auto:<plugin>`, in `InstanceRegistry` but NOT
`project_.instances`). Its capabilities register with no project open; the
provider persists across project open/close and is swept at shutdown.

- **Deployment opt-in, default OFF.** Gated on `--autoload-lib` / env
  `XINSP2_AUTOLOAD_LIB`. This is deliberate: autoloading imgcodec makes BOTH
  `xi.image.decode` AND `xi.jpeg.encode` available, and expose's E2 preview keys
  off `xi.jpeg.encode` availability — so an *implicit* machine-wide autoload
  would silently flip every deployment's WS wire to JPEG previews. The flag
  keeps a stock deployment byte-identical; a deployment that wants the E1 cure
  (and the E2 previews it implies) opts in once. The `"autoload"` marker is
  *eligibility*; the flag is *activation*.
- **Project precedence.** An explicit project instance of an autoload plugin
  wins: before its factory runs (`create_instance` / open_project), the machine
  provider is evicted (adapter dtor owner-sweeps its registrations) so the
  project instance claims the capability names cleanly — no ETAKEN
  double-register. On teardown (`remove_instance` / `close_project`) the machine
  provider is reinstated (its global DLL is never project-unloaded). So E1 step
  (b-i) — a per-project instance — stays a valid alternative to (b-ii) autoload.
- **Machine-scoped recovery.** A project instance re-enables after a quarantine
  by re-committing its config (`set_inst_state → Active`); a machine provider has
  no project commit surface, so `PluginManager::reload_machine_provider(plugin)`
  rebuilds it from a fresh factory (the machine analogue of the re-commit). The
  autoload instance keeps the plugin's declared `on_fault` (imgcodec: `refuse` →
  quarantine on a handler crash), so an unattended deployment that prefers
  self-healing can set the eligible provider's default to `reinit` instead.
- **Proof (all green):** backend ctest `cap_autoload_test` (available with no
  project, project-instance precedence evict+reinstate, close_project
  reinstatement, crash→quarantine→`reload_machine_provider` recovery, teardown
  owner sweep, all via the real `PluginManager` + real lib DLL); QA
  `qa_cap_imgcodec_autoload` (live service, `--autoload-lib`, NO codec instance
  declared — the consumer's door is still served, `enc==1`) alongside the
  unchanged `qa_cap_imgcodec` (WITH a codec instance — precedence: the project
  instance is the provider, `registered==true`).
- **v12 formalization:** promote `--autoload-lib` into declared config (a
  deployment/backend config list) and decide whether autoload should be the
  default once `read_image_file`'s built-in stb fallback is deleted at the cut
  (below) — at that point a deployment with no decode provider has no safety net,
  so autoload-on may become the sane default.

**v12 must revisit**: promote/retire the two interfaces formally; **pay the
`read_image_file` eviction — delete the host ABI slot AND core's built-in stb
fallback, leaving the `xi.image.decode` capability as the only decode engine**
(the pre-v12 slot keeps the stb fallback only so a project with no `imgcodec`
instance still decodes; at the cut that safety net goes with the slot); expose
a stable sealed-image identity across the
ABI (content-hash keying is correct but pays a per-call hash over pixels);
per-capability call counts/latency in dispatch_stats (the pilot funnel does
not meter yet); whether `xi_cap_v1.call` should carry a consumer-declared
timeout; **custom ext type with registered retain/release/dump hooks** — one
mechanism upgrading toolbox handles AND device buffers to registry-grade
lifetime (today the resource-handle convention below fakes lifetime with a
ring/generation lease entirely inside the owner; a registered ext type would
let the pack layer itself retain/release owner objects the way it does pool
images, and `dump` becomes the registered materializer instead of a
convention). Sits alongside the stable-image-identity item — both are "give
the pack plane real identity for things it currently only names".

## Appendix: the resource-handle convention (type-owner lib plugins)

Status: **maintainer-settled; demo landed on the polaris2 line** (pre-v12).
Executable reference: `plugins/lut_owner` (the `demo.lut` type owner), ctest
`cap_lut_owner_test`, QA `examples/qa_resource_handle` — all green. `demo.lut`
is exemplar-grade, not a roster member.

Some plugins own **heavy custom data types** — build-once-query-many
structures (indexes, lookup tables, calibration meshes, model weights) that
are expensive to construct and nonsensical to serialize per hop. These do
**not** ride packs. Instead:

> A **type-owner lib plugin** constructs and destructs them; packs carry only
> the **handle entry**, a nested canonical-mp map:
> `{ "type": "<ns>", "id": <i64>, "gen": <i64>, "$v": <i64> }`.

The owner registers the type's whole verb set as capabilities (for `demo.lut`:
`demo.lut.build`, `demo.lut.query`, `demo.lut.dump`). Consumers hop the entry
through doors like any other pack entry and hand it back to the owner by
capability name. **Sizing doctrine applies unchanged**: a LIGHT object should
just be a canonical-mp schema riding the pack — the handle pattern exists for
objects where one construction amortizes many queries, never as a general
object-passing mechanism.

### The five rules

1. **All alloc/free inside the owner's DLL.** The object never crosses the
   ABI; only handle entries and query answers do. (The per-DLL-singleton
   lesson — a foreign deleter is a layout bomb.)
2. **Immutable after construction** — the seal-semantics extension. Mutation
   = build a new object (and get a new handle). This is what makes concurrent
   consumers, dedup, and byte-deterministic dumps trivially sound.
3. **Lifetime = ring/generation lease (pre-v12).** The owner keeps a ring of
   N slots; under pressure it recycles (LRU in the demo) and **bumps the slot
   generation from a monotonic, never-reused source** — never reused even
   across instance reinit, so a stale handle can never alias a fresh object.
   A resolve against a recycled lease answers a normal sealed
   `$fault "stale_handle"` pack (funnel rc stays 0 — staleness is capability
   contract, not transport verdict). Owner sweep on crash: the ring dies with
   the instance (its on_fault policy governs, exactly like any lib plugin;
   capability registrations are owner-swept as usual), and consumers' held
   handles fail closed — -3 while quarantined, `stale_handle` after a rebuild.
4. **Handle entries are RUNTIME-ONLY — never persisted.** The type owner
   registers a **dump capability as the materializer** (`demo.lut.dump`:
   handle → byte-deterministic canonical bin). A persist sink either
   materializes the record on persist or drops the entry (configured
   sink-side); it never stores the handle itself.
5. **Wrong-type resolve → `$fault "wrong_type"`.** A handle is only
   meaningful to its owning namespace; owners must check `type` before `id`.

### What the demo proves (all green)

`cap_lut_owner_test` + `qa_resource_handle`: build → handle entry; content-
keyed build dedup (two consumers, one sealed content, **build counter pinned
at 1** — the zero-rebuild headline); the entry riding packs through a real
door hop to a second consumer; dump byte-determinism across consumers and
across recycles; ring-pressure recycle and `recycle_all` both → clean
`stale_handle`; foreign-namespace handle → `wrong_type`; malformed /
out-of-range handles → `bad_handle`; handle-`$v` gate; generations strictly
increasing across rebuilds.

### The GPU/VRAM variant (sketch — discussion-grade, NOT scheduled)

The same convention extends to device memory with a **device pool owner** —
a type-owner lib plugin owning a VRAM arena (e.g. `gpu.buf`): CUDA/D3D12
allocations never cross the ABI; packs carry
`{ "type": "gpu.buf", "id", "gen", "$v", "dev": <ordinal>, "event": <i64> }`.
Two deltas on top of the five rules:

- **`event` field for sync**: the producer records a fence/event id at
  enqueue; a consumer capability waits on it before touching the buffer —
  ordering rides in the handle entry, so CPU-side pack hops never
  synchronize the device.
- **Materialize-on-persist is a device→host readback** (the dump capability),
  which is exactly why rule 4 matters: persisting a VRAM id is meaningless
  across runs, but the materializer makes persistence well-defined at a cost
  the sink opts into.

Everything else carries over verbatim: immutable-after-construction (compute
into a new buffer), ring/generation lease over the arena, owner-sweep =
device pool teardown with the instance, wrong-type refusal. **Not scheduled**:
no in-tree consumer needs device residency yet; when one arrives, this
appendix plus the v12 "registered retain/release/dump hooks" item above is
the design seed, not a commitment.
