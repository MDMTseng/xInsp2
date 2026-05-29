# Tier1 + A2 hybrid — Implementation plan

> **SUPERSEDED / REMOVED 2026-05** — the SHM allocator was removed with the FE/BE in-process split. Retained for historical reference.

Companion to `shm-allocator-formal-spec.md`. Decomposes the spec into
~50 concrete impl steps grouped into 7 PR-sized phases. Each step is
small enough to commit individually if needed.

Top-level task tracking lives in `TaskList` (#85-#92). This doc is
the punch-list inside each phase.

## Phase boundaries (one PR per phase unless noted)

```
A. Header extensions          (no behaviour change; safe to ship alone)
B. Layer A2 (shared buckets)  (closes P0-E1 partially; behaviour change)
C. Layer B (ownership)        (closes C-P1-6; ships with B or right after)
D. Layer A1 (private pools)   (perf + provisioning; opt-in per plugin)
E. Telemetry                  (operational; can parallel B/C/D)
F. Q2 retention               (independent; any time)
G. Stress + race tests        (after B, C, E land)
```

Critical path: A → B → C → D. E/F/G can fan out.

---

## Decisions — locked 2026-05-10

Discussed and signed off; defaults below, project-level overrides
where noted.

| # | Decision | Locked value | Notes |
|---|----------|-------------|-------|
| **D1** | Bucket sizes | **`[16 MB, 64 MB, 256 MB]`** (3 buckets) | **Project-configurable** via `project.json` `shm.buckets_mb`. Covers 5 MP color (15 MB → 16 MB bucket), 20 MP color (60 MB → 64 MB), 50 MP color (150 MB → 256 MB) |
| **D2** | Row alignment `kRowAlign` | **64 bytes** | AVX-512 / cache line / typical GPU coalescing |
| **D3** | API naming | **Keep `shm_create_image` as primary**; add **`image_create_smart`** helper that auto-decides SHM vs heap by size | Existing plugins keep working unchanged. Layer B (ownership transfer) documented in comment block, not API rename |
| **D4** | A3 overflow when region full | **fail** (alloc returns INVALID) | Plugin / worker decides whether to drop the frame or fall back to heap |
| **D5** | A1 reservation strategy | **scatter** (each instance pool drawn from A2) | Simpler boundary tracking; cache-locality penalty negligible at our chunk sizes (≥16 MB) |
| **D6** | Hot-reload A1 | **re-provision** | Existing instance dtor + recreate flow already exercises this |
| **D7** | A3-origin release | **no-op** | A3 is bump fallback; refcount→0 means block stays orphaned until region reset. Telemetry counter alerts if A3 path is hot |

### Additional decisions (project-configurable)

| # | Knob | Default | `project.json` path |
|---|------|---------|---------------------|
| **D8** | SHM region size | **512 MB** | `shm.region_size_mb` |
| **D9** | Promote threshold | **16 MB** (matches MAX_PAYLOAD_BYTES) | `shm.promote_threshold_bytes` |
| **D10** | Per-instance default `max_in_flight` | **3** | `shm.max_in_flight_per_instance`; per-output overrides via plugin manifest `outputs[].max_in_flight` |
| **D11** | Metadata layout | **separate** (current — Image in SHM, JSON over wire IPC) | Inline-metadata path deferred to a future SHM_VERSION bump if profiling shows IPC JSON is a bottleneck |

### Workload sizing reference

```
4 cameras × 7 fps, color (×3 channels), per-frame size:

       resolution  per-frame  bucket   4cam × 3 in-flight × frame  region needed
  ──────────────────────────────────────────────────────────────────────────────
       5 MP        15 MB       16 MB   180 MB                       512 MB OK
      20 MP        60 MB       64 MB   720 MB                       1 GB recommended
      50 MP       150 MB      256 MB   1.8 GB                       2 GB required
```

For the **future 50 MP × 4 cam** workload, set `shm.region_size_mb: 2048`.

---

## Phase A — Header extensions + ShmConfig plumbing (no behaviour change)

Goal: lay down the data structures + project-level configuration
plumbing so subsequent phases have a place to land. Existing alloc/
release behaviour unchanged; new fields all default to "shared / A2
origin" so nothing observes a difference. ShmConfig is read at
backend startup but doesn't affect bump-only behaviour yet.

| Step | Description | Files |
|------|-------------|-------|
| A.1 | Add `stride`, `tier_origin`, `_pad_byte[3]`, `owner_instance_id`, `next_free_offset` to `ShmBlockHeader` | `xi_shm.hpp` |
| A.2 | Static-assert `sizeof(ShmBlockHeader) == 64`; rebalance `_reserved[]` | `xi_shm.hpp` |
| A.3 | Add `ShmMetrics` struct (atomic counters) and `free_head[N_BUCKETS_MAX]` to `ShmRegionHeader` | `xi_shm.hpp` |
| A.4 | Bump `SHM_VERSION` to 2; existing version handshake (PR #50) catches mismatched binaries automatically | `xi_shm.hpp` |
| A.5 | Define `struct ShmConfig { region_size_mb, promote_threshold_bytes, buckets_mb[], max_in_flight_per_instance }` with hard-coded defaults `[16, 64, 256]` MB / 16 MB / 512 MB region / 3 in-flight | `xi_shm.hpp` |
| A.6 | Add `parse_shm_config(cJSON* project_root) -> ShmConfig` that reads optional `shm` section; missing keys → defaults | `xi_plugin_manager.hpp` |
| A.7 | Plumb `ShmConfig` through `ShmRegion::create(name, config)`; store config in region header so worker `attach()` can read same values | `xi_shm.hpp` + `service_main.cpp` |
| A.8 | Update existing `alloc_()` to set `tier_origin = A2`, `owner_instance_id = 0`, `next_free_offset = 0`, `stride = w * ch` (placeholder, B fixes proper alignment) | `xi_shm.hpp` |
| A.9 | Update `alloc_image` / `alloc_buffer` signatures to take optional `owner_id` (default 0); thread through `alloc_()` | `xi_shm.hpp` |
| A.10 | Update `host_api->shm_create_image` / `shm_alloc_buffer` to forward owner_id; default 0 for back-compat | `xi_image_pool.hpp` |
| A.11 | Add helper `xi::ShmRegion::block_header(handle)` returning const-ref so callers can read tier_origin, stride etc. | `xi_shm.hpp` |
| A.12 | Doc the `shm` block in `docs/protocol.md` (under project.json schema) | `docs/protocol.md` |
| A.13 | Build clean (Release + tests); run fl_r8 full + cross_proc_trigger + multi_source_surge → expect 0 regression | (CI) |
| A.14 | PR title: "SHM header extensions + project-configurable shm block (no behaviour change)" | (git) |

**Acceptance**: build clean; existing examples PASS; on-disk SHM region size matches config (default 512 MB); `project.json` accepts optional `shm` block. All headers extended, no allocator behaviour changed yet.

---

## Phase B — Layer A2 (shared size-class free-list)

Goal: replace bump-only with reclaiming size-class allocator. Closes
P0-E1 (region exhaustion). Layer B not yet wired, so still uses
old worker release semantics — **acceptable because bump-only's
"refcount=0 keeps bytes valid" guarantee is preserved up to the
moment a recycled block is overwritten by a new alloc**. Reduce that
window by deferring reuse with a small "quarantine" delay if
needed (see B.13).

| Step | Description | Files |
|------|-------------|-------|
| B.1 | Read bucket sizes from `ShmConfig.buckets_mb` (set up in Phase A); compile-time `N_BUCKETS_MAX = 8` cap to keep `free_head[]` array bounded; runtime `n_buckets` reflects actual config | `xi_shm.hpp` |
| B.2 | `bucket_index(size_t s) -> int`: linear scan through configured buckets; returns smallest i where `BUCKET_SIZES_RUNTIME[i] >= s`; -1 if too big (caller falls to A3) | `xi_shm.hpp` |
| B.3 | Define `kRowAlign = 64`; `align_up(size, align)` helper | `xi_shm.hpp` |
| B.4 | Update `alloc_image(w,h,ch)` to compute `stride := align_up(w*ch, kRowAlign)`, `payload_size := stride * h`, then `bucket := bucket_index(payload_size)` | `xi_shm.hpp` |
| B.5 | Implement `push_free_list(bucket, offset)`: 64-bit CAS on `free_head[bucket]`; encode `(tag : u16, offset : u48)`; tag monotonically incremented per push | `xi_shm.hpp` |
| B.6 | Implement `pop_free_list(bucket) -> offset (0 if empty)`: CAS-pop with ABA defense; reads `next_free_offset` from popped block to update head | `xi_shm.hpp` |
| B.7 | Refactor `alloc_(kind, ..., bucket)`: try pop_free_list; on miss, bump-allocate `BUCKET_SIZES[bucket] + sizeof(ShmBlockHeader)`; on bump fail return A3-tagged or INVALID per D4 | `xi_shm.hpp` |
| B.8 | Update `release(H)`: when `refcount → 0`, read `tier_origin` and `bucket_index(payload_size)` from header, call `push_free_list(bucket, offset)` | `xi_shm.hpp` |
| B.9 | Update `alloc_buffer(size)` to also bucket; small Buffer (e.g. 1 KB metadata) lands in 256 KB bucket = 256× overhead. Acceptable for v1; flag for D1 reconsideration if Buffer becomes hot. | `xi_shm.hpp` |
| B.10 | Add `XINSP2_SHM_NO_FREELIST=1` env var that disables free-list (forces bump) — escape hatch if prod bug found, can fall back to bump-only at startup. | `xi_shm.hpp` + `service_main.cpp` |
| B.11 | Unit tests in `backend/tests/test_xi_shm.cpp`: alloc-release-alloc returns same offset; bucket boundary cases (size==BUCKET_SIZES[i]); ABA tag increments | (new tests) |
| B.12 | Stress test: 16 threads × 100K alloc/release, verify no leaks (block_count after = before), no torn refcount | (new test) |
| B.13 | (optional) "Quarantine" mode: enqueue freed blocks to a per-bucket FIFO; pop only after N pushes have followed (delays reuse). Provides a soft Layer B compensation until C ships. Off by default; flag to enable. | `xi_shm.hpp` |
| B.14 | fl_r8 SHM exhaustion soak: 60 fps × 1 MB × 60 s; assert bump_offset bounded | `examples/fl_r8_concurrency/` |
| B.15 | PR title: "SHM Layer A2: size-class free-list allocator (closes P0-E1)" | (git) |

**Acceptance**: 60 s × 60 fps soak with isolated source — `bump_offset` stops climbing after warmup (visible via shm_metrics or stderr); no torn images observed under quarantine mode; existing examples PASS.

---

## Phase C — Layer B (ownership transfer)

Goal: eliminate the race window `W` from spec §6 so Layer A2 reuse
is safe. Worker stops releasing on send; host stops addref'ing on
receive; both sides add explicit compensation paths.

| Step | Description | Files |
|------|-------------|-------|
| C.1 | Audit `image_release` calls in `worker_main.cpp` emit_trigger lambda | `worker_main.cpp` |
| C.2 | Remove worker-side `image_release(temp_shm)` after `send_frame` success | `worker_main.cpp` |
| C.3 | Add `image_release(temp_shm)` in `send_frame` failure catch block (compensation) | `worker_main.cpp` |
| C.4 | Audit `addref` calls in `xi_trigger_bus.hpp::emit()` (Any / AllRequired / LeaderFollowers branches) | `xi_trigger_bus.hpp` |
| C.5 | Remove `ImagePool::instance().addref(...)` on incoming wire frame entries | `xi_trigger_bus.hpp` |
| C.6 | Add `ImagePool::instance().release(...)` on no-sink drop path so dropped events don't leak | `xi_trigger_bus.hpp` |
| C.7 | Recorder observer: previously borrowed bus's addref'd handle; now must explicitly addref since bus no longer addrefs | `xi_trigger_recorder.hpp` |
| C.8 | Audit any other emit / on_open / observer paths for stale addref/release pairs | (codebase grep) |
| C.9 | Race harness: worker emits image with sentinel pattern in payload; host inspect script reads payload and asserts pattern; loop 10K iters under PageHeap. New example: `examples/fl_r10_ownership_race/` | (new example) |
| C.10 | Disable B.13 quarantine mode (no longer needed; Layer B provides the guarantee) | `xi_shm.hpp` |
| C.11 | Run fl_r8 full + ownership race harness 10K iters; expect 0 torn-image findings | (CI) |
| C.12 | PR title: "SHM Layer B: ownership transfer protocol (closes C-P1-6)" | (git) |

**Acceptance**: race harness 10K iters / 0 torn images; fl_r8 5/5 PASS; cross_proc_trigger + multi_source_surge PASS.

---

## Phase D — Layer A1 (instance-private static pool)

Goal: pre-allocate per-instance pools at project_open for plugins
that declare expected output shapes. Backwards-compatible: plugins
without `size_hint` still work via A2.

| Step | Description | Files |
|------|-------------|-------|
| D.1 | Extend `plugin.json` schema doc: `outputs[].size_hint = {w, h, ch}` (Image) or `{bytes}` (Buffer); `outputs[].max_in_flight = int`; `outputs[].variable = bool` (opt-out) | `docs/reference/plugin-abi.md` |
| D.2 | Update manifest parser in `xi_plugin_manager.hpp::parse_manifest` to read these fields into `OutputSpec { size, max_in_flight, variable }` | `xi_plugin_manager.hpp` |
| D.3 | Define `InstancePoolEntry`: `{ owner_instance_id : u32, bucket_idx : int, free_head : atomic<u64>, chunk_count : int }` | `xi_shm.hpp` |
| D.4 | Add `instance_pools` array to `ShmRegionHeader`. Sized for up to e.g. 64 instances × 8 outputs = 512 entries. | `xi_shm.hpp` |
| D.5 | At project_open: walk each instance × static output, allocate `max_in_flight` chunks from A2 (so they're properly sized), tag each block's `tier_origin = A1` + `owner_instance_id = N`, push to instance pool's free list | `xi_plugin_manager.hpp` |
| D.6 | Allocator `alloc(size, owner)` flow: if owner != 0, find owner's pool for the right bucket, try CAS-pop. On miss, fall through to A2. | `xi_shm.hpp` |
| D.7 | `release(H)` dispatch by `tier_origin`: A1 → push to instance pool; A2 → push to shared bucket. Header carries enough info to route. | `xi_shm.hpp` |
| D.8 | At instance destroy / project_close: walk instance's pool free-list, push all chunks to A2 (re-tag tier_origin=A2). Doesn't matter if some are still in flight (refcount > 0); they'll re-tag on their natural release. | `xi_plugin_manager.hpp` |
| D.9 | Hot reload: on `recompile_project_plugin`, the existing flow destroys + re-creates instance; D.5's project_open path runs again on re-create | (works automatically) |
| D.10 | If A1 reservation fails (insufficient SHM at boot): fail open_project with `"instance X needs A bytes; only B free in region (configure XINSP2_SHM_SIZE)"` | `xi_plugin_manager.hpp` |
| D.11 | Update `examples/parallel_inspect_demo/plugins/burst_source/plugin.json` with size_hint annotation; verify `shm_metrics` shows A1 hit rate >99% | `examples/parallel_inspect_demo/` |
| D.12 | Doc the size_hint convention in `docs/guides/adding-a-plugin.md` and `docs/reference/plugin-abi.md` | (docs) |
| D.13 | PR title: "SHM Layer A1: instance-private static pools" | (git) |

**Acceptance**: parallel_inspect_demo with size_hint shows >99% A1 hits; instance death + re-create cleanly returns chunks to A2 then re-reserves; multi_source_surge stays PASS.

---

## Phase E — Telemetry

Goal: production observability. Can ship in parallel with B (no Phase
B/C dependency on the metrics struct existence — A.3 already added it).

| Step | Description | Files |
|------|-------------|-------|
| E.1 | Define `ShmMetrics` fields (already in A.3 — confirm: `a1_acquire_total`, `a2_acquire_total`, `a3_bump_total`, `alloc_failed_total`, `a2_free_count[N_BUCKETS]`) | `xi_shm.hpp` |
| E.2 | Wire `alloc_()` to bump `a1_acquire_total` / `a2_acquire_total` / `a3_bump_total` based on which path served the request | `xi_shm.hpp` |
| E.3 | Wire `push_free_list` / `pop_free_list` to update `a2_free_count[bucket]` | `xi_shm.hpp` |
| E.4 | Wire `alloc_failed_total` on INVALID return (per D4 fail policy) | `xi_shm.hpp` |
| E.5 | Add `cmd:shm_metrics` handler in `service_main.cpp`: returns JSON snapshot of all atomic counters + per-bucket free_count + per-instance A1 utilization | `service_main.cpp` |
| E.6 | Doc `cmd:shm_metrics` in `docs/protocol.md` (reply shape) | (docs) |
| E.7 | Add `Client.shm_metrics()` helper to Python SDK | `tools/xinsp2_py/xinsp2/client.py` |
| E.8 | Test: smoke harness exercises A1/A2/A3 paths; asserts each counter increments | (new test) |
| E.9 | Add Grafana-style alarm-rule examples in docs (optional): `a3_bump_total > 0`, `a2_free_count[1MB] / total < 0.2`, `alloc_failed_total > 0` | `docs/operations.md` (new) |
| E.10 | PR title: "SHM telemetry counters + cmd:shm_metrics" | (git) |

**Acceptance**: shm_metrics returns sensible numbers under each Tier hit path; Python helper roundtrips; all counters monotonic.

---

## Phase F — Q2: script_build/ retention

Goal: stop disk-fill from accumulated `_v<N>.dll/.lib/.obj/.log`.
Independent — can ship anytime.

| Step | Description | Files |
|------|-------------|-------|
| F.1 | After successful `xi::script::compile`, scan output_dir for `<stem>_v<N>.{dll,lib,obj,log,exp,pdb}` files | `xi_script_compiler.hpp` |
| F.2 | Sort by N (extract from filename); keep latest, delete the rest | `xi_script_compiler.hpp` |
| F.3 | Same logic for `xi_plugin_manager::compile_project_plugins_locked` (per-plugin build/ folder) | `xi_plugin_manager.hpp` |
| F.4 | Use `std::error_code` overload of `fs::remove` so a failed delete (e.g. AV scanner has the file open) doesn't break compile | (both) |
| F.5 | Test: trigger 5 hot-reloads; assert only `_v5.{dll,lib,obj,log}` remains | (new test or example) |
| F.6 | Doc the retention policy in `docs/protocol.md` § compile_and_load + `docs/reference/plugin-abi.md` | (docs) |
| F.7 | PR title: "compile: delete stale _vN artifacts on success" | (git) |

**Acceptance**: 30-day worth of dev-loop hot-reload simulated by 100x compile_and_load yields ≤ a single live version on disk.

---

## Phase G — Stress + race tests

Goal: production-quality validation of the new allocator + ownership
protocol. Run after B and E (so A2 + telemetry exist) and after C
(so ownership transfer is in place).

| Step | Description | Files |
|------|-------------|-------|
| G.1 | New example `examples/fl_r10_shm_soak/`: open project with 1 isolated source @ 60 fps × 1 MB; run 60 s; assert: bump_offset bounded after warmup, a3_bump_total stays 0 (or below threshold), 0 torn images | (new example) |
| G.2 | Race harness (consolidate from C.9): worker stamps sentinel `0xDEADBEEF` + sequence in payload; host inspect verifies bytes match sequence; loop 10K under PageHeap | (new example) |
| G.3 | A1 hit rate test: parallel_inspect_demo, sample shm_metrics every 100 ms, assert `a1_acquire_total / (a1+a2+a3) > 0.99` after warmup | (new example) |
| G.4 | Multi-instance contention: 8 isolated sources, same bucket size, 60 fps each. Assert no CAS livelock (alloc latency p99 < 1 ms), telemetry counters all monotonic | (new example) |
| G.5 | Buffer + Image mixed: half the sources emit images, half emit buffers (different bucket). Assert no cross-bucket interference | (new example) |
| G.6 | Bump fallback test: pre-fill A2 + A1, force one alloc to fall through to A3; assert `a3_bump_total += 1` and alloc returns valid handle | (new example) |
| G.7 | Region exhaustion at boot: configure SHM region too small for an instance's A1 pool; assert `open_project` fails fast with the right error message (per D.10) | (new example) |
| G.8 | PR title: "FL r10 stress + race tests for tiered SHM allocator" | (git) |

**Acceptance**: all G.1-G.7 pass; CI gate (or manual) runs them on every SHM-touching PR going forward.

---

## Step count summary

| Phase | Steps | PR |
|-------|-------|----|
| A | 10 | 1 |
| B | 15 | 1 (or split B.1-B.10 + B.11-B.15) |
| C | 12 | 1 |
| D | 13 | 1 |
| E | 10 | 1 |
| F | 7 | 1 |
| G | 8 | 1 |
| **Total** | **75 steps** | **7 PRs** |

Bigger than the rough "8 phases" estimate but still bounded.

## Estimated effort

Eyeballing under "I-know-the-codebase-well" assumption:

```
A : 0.5 days
B : 1.5 days  (CAS + ABA tag is the hardest part)
C : 0.5 days  (audit + delete release calls + race harness)
D : 1.5 days  (manifest schema + project_open reservation + dispatch logic)
E : 0.5 days  (mechanical)
F : 0.25 days
G : 1.0 days  (writing 7 harnesses)
─────
~5.75 days end-to-end serial.
```

Critical path A→B→C→D = 4 days. E/F/G can overlap. Realistic
calendar: **5-7 days** with regression checks + PR review cycles.

## Risk register

| Risk | Mitigation |
|------|-----------|
| ABA bug in CAS-pop (silent corruption) | B.11 unit test exercises the ABA path explicitly with stress; PageHeap on Win32 catches use-after-free |
| Layer B audit misses a release/addref site | C.8 codebase-wide grep; G.2 race harness catches torn images empirically |
| A1 reservation runs out of SHM region | D.10 fast-fail at boot with actionable error |
| Old plugins break on `tier_origin` field addition | A.5 defaults `tier_origin = A2` for any block alloc'd without explicit owner_id; SHM_VERSION bump caught by handshake (PR #50) |
| Buffer (small metadata) wastes 250× memory in 256 KB bucket | D1 reconsidered if profiling shows it matters; can add 4 KB / 64 KB buckets cheaply |
| `quarantine` mode (B.13) hides a real bug from C | Default off after C lands; comment block flags it as transitional |
