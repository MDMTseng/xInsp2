# 19 — Hardening backlog

Status: **living backlog** (curated 2026-07-04). Items surfaced by the polaris2
example/eviction waves and the design discussions, collected here so nothing is
lost between waves. Grouped by disposition; each has a severity and an effort
class (**S** <1h · **M** hours–day · **L** days). Nothing here blocks THE CUT —
these are quality items, and the v12 rows are already reflected in doc 06 §1 /
doc 10 / doc 14.

## A. Schedule next — real defects with a clear fix

| ID | Item | Where | Severity | Effort |
|---|---|---|---|---|
| H1 | **`t.has_source()` is blind to pack-plane triggers.** The `data_` (explicit-trigger) path keys off `data_->images` — the Record-plane image map, which is EMPTY for a pack emit — so `has_source()` always answers false on the pack path. Multi-source pack routing must use `t.primary_source()` today. Fix: also consult the pack's source/channel identity (or `leader_source`) on the pack path. | `backend/include/xi/xi_use.hpp:637` (found by qa_multi_graph) | Medium — silent wrong-false in multi-source pack scripts | S–M |
| H2 | **`project.json` `instances` array is silently inert.** Instances materialize ONLY from `instances/<name>/instance.json`; a `project.json`-only entry does nothing and emits no warning. Fix: warn loudly on a project.json instance with no backing dir (or honor the array). | project loader (found by qa_multi_graph) | Medium — silent misconfiguration | S |
| ~~H3~~ | ~~**QA drivers share default port 7917 → squatter cross-talk.**~~ **DONE** (polaris2/h-qa-infra). Swept **38** `examples/qa_*/driver.py` onto `examples/lib/ports.free_port()` (matching the qa_pack_feedback exemplar: `int(os.environ.get("PORT","0")) or free_port()`, or a bare `free_port()` where there was no env override). Also killed the related **derived-port** anti-pattern — `qa_dispatch_groups` (`PORT+1..+5`) and `qa_run_result` (`PORT+1`) now allocate an independent `free_port()` per backend (a `+5` offset off an ephemeral base is not guaranteed free — it hit a live "failed to bind" during the proof run). | `examples/qa_*/driver.py` | Medium | S (sweep) — landed |
| ~~H4~~ | ~~**`qa_recipe_script_instance`** — after reset, `factor` stays 42.~~ **DONE / not a backend bug** (polaris2/h-qa-infra). Diagnosed by the backend sibling: the driver's step-4 "reset" was a second `compile_and_load` — a hot-recompile that DELIBERATELY replays the cached tuned instance defs (`g_eng.instance_def_cache`) so a recompile never wipes operator tuning, so `factor` staying 42 is by design. Fixed the driver to use a genuine reset (`close_project` + reopen + compile → factor back to 5); also fixed step-6, which needed catching `PartialStatusError` (the client raises on a `partial` load, and this recipe deliberately induces one via a missing ghost instance) to read `instance_warnings`. Driver now PASSes all 3 assertions; quarantine entry removed. | `examples/qa_recipe_script_instance/driver.py` | Medium — was a stale test driver, not a reset-semantics bug | S |
| ~~H5~~ | ~~**`qa_local_auto`** — driver expects VAR-era auto-emitted run_results.~~ **DONE / diagnosis was wrong** (polaris2/h-qa-infra). The driver was already on the current run_result contract (identical to green qa_run_result / qa_emit_frame_key). The real defect: `instances/cam/instance.json` pointed `dir` at `examples/object_count_solve/frames`, an example that was deleted — 0 images → 0 auto-emits → 0 run_results. Repointed to `examples/blob_tracker/frames` (as the sibling qa_emit_frame_key does); driver now PASSes (10 run_results, ok=10). Quarantine entry removed. | `examples/qa_local_auto/instances/cam/instance.json` | Low — dead config path | S |

## B. v12 cutover-train items (deferred, coordinated — see doc 06 §1 / 10 / 14)

| ID | Item | Rationale for deferral |
|---|---|---|
| V1 | **Core JPEG ENCODER eviction** — `xi_jpeg.hpp` / `compress_sink` (the host preview-compress cache) migrates to `xi.jpeg.encode`. | Deferred from the decode eviction: it's a hot path with its OWN content-hash memo cache; routing through imgcodec's cache would double-cache and change identity/threading. Do it with V2 at the cut. |
| V2 | **Delete `read_image_file` slot + core stb fallback** — imgcodec becomes the only decode engine. | Already staged: pre-v12 the slot delegates + falls back (doc 06 §1 row 8). The deletion is the v11→v12 break. |
| V3 | **Lib-plugin machine-level autoload** — a capability provider available without a per-project instance. | Today an `imgcodec` instance must exist per project (E1). A machine/host-level registration would remove that step. doc 14 candidate. |
| V4 | **Custom ext-type with registered retain/release/dump hooks** — one mechanism giving toolbox handles AND device buffers registry-grade lifetime. | doc 14 v12 must-revisit; unifies the resource-handle convention (doc 14 appendix) beyond the pre-v12 ring/generation lease. |
| V5 | **Stable sealed-image identity across the ABI** — so imgcodec keys its cache on handle id+generation instead of a per-call content hash. | doc 14 v12 item; the content-hash is a pre-v12 workaround. |

## C. Robustness residuals — low severity, tracked not urgent

| ID | Item | Note |
|---|---|---|
| R1 | **PackRegistry owner-ledger mis-attribution** — a tagged ref released from a guard-less thread while multiple owners hold buckets can be mis-attributed in the ledger. | NEVER memory-unsafe, NEVER over-released (handles fail closed); documented in `xi_pack_abi.hpp`. Tighten if a real leak is observed. |
| R2 | **Capability funnel not metered into `dispatch_stats`** — doc 14 promised call-count/latency observability "for free"; the pilot didn't wire it. | Cheap add when the plane grows; needed before the plane carries production load. |
| R3 | **Codegen `.gen.ts` / `_gen.py` emitters have no consumer** — `gen_contract.py` emits a per-plugin TS/Py pair nobody imports yet; deleting them fails the codegen-equiv drift gate. | Report-not-delete (verified orphan-but-load-bearing-to-the-gate). If never adopted, remove the emitters from `ARTIFACTS` for all plugins at once — a generator decision, not a file delete. |

## D. Discussion-only — design recorded, NOT scheduled

| ID | Item | Where the thinking lives |
|---|---|---|
| D1 | **Reemit / mux priority plugin** — lane priority via a sorting sink that gathers A/B lanes and re-emits in preset order (windowed; no preemption). Sizing: a normal data-plane plugin, not a lib plugin. | This session's discussion; U3 ordering contract (doc 17) makes it expressible without a core change. |
| D2 | **Pinned (page-locked) pool allocation flag** — 2–3× faster + async GPU transfer; pure pool-internal, zero ABI change. | GPU hard-limit discussion (doc 01 hard-limits table). |
| D3 | **GPU-island / resource-handle for VRAM** — device-pool owner lib plugin, `event` fence field, materialize-on-persist. | doc 14 appendix (resource-handle convention, GPU variant, explicitly not scheduled). |

## How to use this

Pull a batch from **section A** (they're independent and mostly S/M — a good
parallel-agent wave). Section B rides the cut. Sections C/D wait for a concrete
trigger. When an item lands, delete its row (or move it to a "done" note); when
a new one surfaces mid-wave, add it here rather than letting it live only in an
agent's final report.
