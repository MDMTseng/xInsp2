# Hot-path performance — findings, baseline, and roadmap

> **Status:** findings-only (nothing here is implemented yet). Captured from the
> 2026-07-10 hot-path scan + `bench_*` baseline.
>
> **[2026-07-11] TypedPack deleted (commit `cba51fe`):** the in-process
> `TypedPack<Schema>`/`TypedPackBuilder` container this page benchmarks was
> deleted (0 production consumers; the `frame_micro_typed_ns` perf baseline was
> retired with it). The `TypedPack` numbers below are kept as the historical
> measurement; items **B-2** and the "steer to TypedPack" half of **B-1** are
> **moot**. Speed-first is spine principle #1
> ("zero-copy, no I/O or allocation on the per-frame path, **measure before you
> trade it away**"), so every item below is a *candidate* to be benchmarked
> before/after, not a mandate.

The lens: on the per-frame hot path, where does the **common** input shape pay a
tax for machinery only a **rare** shape needs? Three response classes:

- **A — fast-path cache** (behavior-preserving): the common case stops paying for
  a branch it doesn't use. No input rejected.
- **B — reject/constrain the rare shape** (input-guard, fail-loud/opt-in): the
  pathological combination is refused at the boundary so the common path stays lean.
- **C — rare is actually normal**: do NOT reject; a real structural fix is required.

## Baseline (this capture machine — per-machine numbers, not portable)

Captured on the dev box, `fix/redteam-round11` (structurally == `polaris2_main`
for these paths). Recapture on the gating machine before trusting absolute numbers;
the shape/ratios are what matter. `perf_gate.cmake` deliberately ships un-baselined
(a per-machine latency is meaningless cross-machine).

### `bench_pack` — metadata plane micro (no images, no dispatch; min-of-batches)

| path | ns/op | composition |
|---|---:|---|
| Dynamic `PackBuilder`+seal+read+drop | **797** | arena build + hybrid index + interned keys + one-shot free |
| `TypedPack` set+seal+slot-read+drop | **524** | arena build (recycled chunk) + slot offset reads — no intern, no lookup |
| mp plane + memcpy-hop + offset-read | 763 | mp build + memcpy hop + offset read + drop |

**Typed is ~273 ns/pack cheaper than dynamic** — the dynamic index + key-intern tax.

### `bench_pack` / `bench_hotpath` — closed-loop service latency (p50, µs)

| workers | pack p50 | hotpath p50 | fps |
|---|---:|---:|---:|
| 1 | 8 | 7 | 29–56k |
| 2 ordered | 4 | 9 | 64–145k |
| 4 ordered | 6 | 9 | ~64–142k |
| 8 ordered | 8 | 9 | ~53–64k |

Overload (heavy inspect > arrival, qd=64, 1 worker): p50 ~1053 µs, p99 ~4456 µs,
drops — i.e. queue-wait tail + overflow shedding, as designed (not a per-frame cost).

### `bench_image_pool` — create + full-buffer memset (single thread)

| size | creates/s | ns/create |
|---|---:|---:|
| 16×16×1 (isolates pool slot+alloc) | 3.9M | **254** |
| 320×240×3 (~230 KB, typical CV frame) | 153k | **6,518** |
| 1280×1024×1 (~1.3 MB) | 2.9k | **344,161** |

The 16×16 case (254 ns) is the pure slot+alloc overhead; the 230 KB (6.5 µs) and
1.3 MB (344 µs) costs are **almost entirely `new PoolEntry` + `vector::resize`
zero-fill** (value-init, then the real pixels overwrite = a redundant N-byte write).
**This is the single largest per-frame reclaim** — see C-1.

## Findings

### C-1 — ImagePool recycles handle *slots*, never *pixel memory* (biggest reclaim)
`xi_image_pool.hpp` `create` does `new PoolEntry()` + `pixels.resize(n)` per create;
`release`→`reclaim_entry_` `delete`s the entry. A per-frame frame buffer (and any
`add_bin` ≥ `kPackLargeThreshold=4096`) therefore pays an allocation **and a
redundant zero-fill** every frame with zero amortization. Bench: 6.5 µs @ 230 KB,
**344 µs @ 1.3 MB** (→ a 1.3 MB source is capped at ~2.9k fps single-thread).
The "rare" shape here (4–64 KB blobs, MB-scale frames) is **normal**, so rejection
is the wrong tool. **Fix (C):** a size-classed pixel-buffer freelist in the pool
(reuse the backing store on release), and `reserve` instead of `resize` so the
buffer isn't zero-filled before the real pixel write. Guard refcount/generation +
concurrency carefully (the pool is lock-free). Measure with `bench_image_pool`.

### A-1 — per-frame group routing pays two global mutexes for single-group projects
`service_dispatch.cpp:678` (`instance_group` under `PluginManager::mu_` + map lookup
+ `std::string` return) then `lane_for_` (`:115-129`, global `lanes_mu` + linear
string-compare scan). Paid **every emit even by the overwhelmingly common
single-`""`-lane project**; the J6 comment already documents this lock on the emit
hot path. **Fix (A):** groups are load-only per spawn — cache a resolved
`weak_ptr<GroupLane>`/lane-index (+ lanes-generation) on the source instance, or an
atomic `single_lane_` fast-path set when `lanes.size()==1` that skips both lookups.
(Note the same staleness discipline as the lane-ABA fix — invalidate on respawn.)

### A-2 — headless (no WS client) still builds 3 JSON events + 3 copies + global mutex per frame
`run_started`/`run_result`/`run_finished` are built (`service_result.cpp:134-168` ≈
12 appends+escapes), `to_json`'d, then copied into the writer buffer under global
`out_mu_` (`xi_ws_server.hpp:1058`) and only then dropped when `client_==INVALID`.
The unattended factory PC is a first-class deployment. **Fix (A):** hoist a
`has_client()` check before building the event strings (metrics recording stays).

### A-3 — TriggerBus takes `mu_` twice per emit + a per-source map stamp
`xi_trigger_bus.hpp:143-157` locks `mu_` to write `source_last_emit_mono_us_[source]`
(string hash) then again to copy the `std::function` sink. **Fix (A):** per-source
atomic liveness slot handed out at registration + an atomic sink pointer (set only at
lifecycle boundaries under quiesce).

### A-4 — declared-sink staging + inspect-wrapper globals
`service_sinks.cpp:160-198` always pays `retain_untagged` + `g_staged.push_back` per
sink push even on an n==1 lane where inline delivery is order-equivalent
(`service_inspect.cpp:71` also takes global `g_eng.script_mu` + copies `LoadedScript`
per frame; `trigger_id_hex` heap-allocates a 32-char string > SSO). **Fix (A):**
deliver `push()` inline when effective worker count == 1; make the script snapshot an
atomic `shared_ptr` load; format `trigger_id_hex` into a stack buffer. (Already
amortized: the EmitTurn gate itself is skipped when `n==1` — `service_dispatch.cpp:368`.)

### B-1 — dynamic pack > 24 keys builds an `unordered_map` at every seal
`xi_pack.hpp:453/557` — 25+ entries → `unordered_map` (~N node allocs) built eagerly
at seal, 100% waste for index-order consumers (record/expose dumpers). **Fix (B):**
cap dynamic per-frame packs at ≤24 keys, or build the
index lazily on first `find`. *(The original "steer wider field sets to a declared
`TypedPack`" option is moot — TypedPack was deleted 2026-07-11, `cba51fe`.)*

### ~~B-2 — undeclared key in a TypedPack silently degrades to the dynamic path~~ (MOOT 2026-07-11 — TypedPack deleted, `cba51fe`)
`xi_pack.hpp:916-946` — an `add_*(key,…)` for a key not in the schema grows a `dyn_`
vector + interns the key, and every subsequent string-key read scans the schema table
then the side list. Silent; profiles show diffuse cost. **Fix (B):** an opt-in
`strict` `TypedPackBuilder` that fails loud on an undeclared key (one compile flag,
breaks only the mistake it targets). Saves the 273 ns/pack dynamic tax too.
*(Resolved by deletion: the whole silent-degrade class is unrepresentable now.)*

### B-3 — `add_str`/`add_mp` bypass the large-object threshold; arena freelist pins by count not bytes
`xi_pack.hpp:636/709` — unlike `add_bin`, no `kPackLargeThreshold` check, so a large
str/mp is a dedicated arena chunk; `ArenaPool::release` retains up to 32 buffers of
**any size** (`:199-206`), so a burst of large entries pins 32 × MB-scale buffers per
lane thread, and first-fit hands a giant buffer to the next tiny pack. **Fix (B, looks
like an oversight):** route `add_str`/`add_mp` ≥ threshold to the pool like `add_bin`;
size-cap the arena freelist (free buffers > e.g. 64 KB outright).

### B-4 — one-shot dispatch path spawns a thread + takes global `run_mu` per frame
`service_dispatch.cpp:517-550` — a source streaming at frame rate while the project was
never `cmd:start`ed pays a thread create + total `run_mu` serialization per trigger
(the one-shot path is for click-driven issue/replay). **Fix (B):** rate-gate it
(refuse > K one-shots/sec with a loud "start continuous mode"), or lazily route
sustained emission into a persistent lane.

### Constants / oversight fixes (cheap, behavior-neutral)
- **`kPackLargeThreshold=4096` vs `kChunk=4096` off-by-header** (`xi_pack.hpp:390/291`):
  a 4092–4095-byte inline entry encodes to 4097–4100 B and can never fit a 4096 head
  chunk → guaranteed `extra_` vector alloc per pack. Set `kPackLargeThreshold ≤
  kChunk − 5 − maxkey` (≈4064) or `kChunk=8192`.

### Adjacent flat costs (not rare-input-gated — out of this scan's brief, flagged for a common-path pass)
- **Every `xi_pack_v1` accessor takes the global `PackRegistry` mutex + hash `find`**
  (`xi_pack_abi.hpp:137`): a plugin reading 20 keys/frame pays 20 lock+lookups. Likely
  the single largest flat per-frame tax; worth a structural look (handle→pointer cache
  for the duration of a call, or a lock-free registry read path).
- `PackBuilder::entries_`/`handles_` vectors are never `reserve`d → growth allocs even
  for the common small pack.

## Roadmap (by reclaim ÷ risk, per the bench)

1. **C-1 ImagePool pixel recycling** — biggest by the numbers (frame-buffer create
   dominates: 6.5 µs–344 µs), but touches the lock-free pool core → its own PR + a
   focused `bench_image_pool` before/after and a concurrency review.
2. ~~**B-2 strict TypedPack + B-1 >24-key steer**~~ — moot 2026-07-11 (TypedPack
   deleted, `cba51fe`); only B-1's lazy-index option remains live.
3. **A-2/A-1/A-3 + B-3 + the kChunk constant** — behavior-preserving fast-path caches;
   biggest under the headless + multi-source shapes; low risk.
4. **B-4 one-shot rate-gate** — a clean reject of a misconfiguration.
5. **Adjacent flat costs (accessor lock)** — structural, needs design.

Each item: capture `bench_pack` / `bench_hotpath` / `bench_image_pool` before, apply,
re-capture, keep the delta in the PR. Do not trade a hot-path cycle without a number.
