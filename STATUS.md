# perf/ws-lean — status & handoff

Branch: `perf/ws-lean` (off `perf/ws-throughput`). This branch is the SOLE landing
path for `perf/ws-throughput` (it lands transitively). Worktree: `.claude/worktrees/xr-02`.

## Done (committed, gate-green)

Six commits (`git log --oneline perf/ws-throughput..HEAD`):
1. `perf(ws)` recycle outbound frame buffers (page-zero alloc kill)
2. `perf(expose)` seed XEX1 magic into encoder buffer (kill prepend copy)
3. `perf(abi/ws)` xi.emit@2 zero-copy owned binary emit (kill the send copy)
4. `feat(sdk)` mint-then-fill producer convention + doc
5. `perf(ws)` adaptive per-connection SNDBUF (restore small-frame wedge sharpness)
6. `test(qa)` bound qa_slow_consumer recv (clean FAIL, not 360s TIMEOUT)

Full design + measurements: **docs/new_gen/32-ws-lean-egress.md** (+ doc 23 adaptive-SNDBUF section).

Headline: backend CPU roughly **HALVED** at 5 MP@60 (3.75 → ~1.8 cores) at equal
throughput. Throughput is **writer/kernel-bound at ~480 MB/s** (this box's raw-
loopback ceiling is 612), so the copy removals cut CPU/bandwidth, not MB/s.

Tests: `ws_async_writer` Phase 4 (owned byte-identity + exactly-once release),
`test_abi_freeze` (xi_emit_v2 pin), `test_interface_domains` (xi.emit@2),
`test_script_pack` (mint_image_blob byte-identical + pool-balanced). Wire identity:
xex1 goldens + record_replay + golden_plugin. qa_slow_consumer solo 2x PASS
(wedge drop 1.8-1.9 s). Full gate: see the final run (was PASS on the pre-SNDBUF
state; re-run pending on final state).

## Remaining — OPTIONAL, needs an orchestrator decision

### Scatter-gather encode (kill the last producer copy)

`expose`'s encode still does ONE 15 MB copy: the blob → the msgpack `Writer`
buffer (`encode_frame_v3`, `w.bin(blob, len)`). Profiled at ~5 ms/frame at 5 MP.

Design (machinery already exists): emit the frame as segments
`[envelope-head-incl-bin-header] + [blob span borrowed from the pack pool buffer]
+ [envelope-tail]` and hand them to the host via the **existing** `xi.emit@2`
`emit_binary_owned` multi-segment path, with the **input pack RETAINED** as the
owner token (release = `pk->release(handle)`). No new ABI. The owned-segment +
RAII-owner-release machinery is done and tested; the new work is (a) a scatter
variant of the encoder that splits the blob's `bin` into header + borrowed span
(byte-identical to the contiguous encode — split at the exact same offset), and
(b) `expose` retaining the pack across the async writer.

Honest cost/benefit — recommend NOT doing it unless a future producer bottleneck
justifies it:
- Throughput is WRITER-bound, so this does NOT raise MB/s (CPU-only: ~5 ms/frame).
- CPU is already halved; this is diminishing returns.
- Risk is the highest of the wave: byte-identity of a hand-interleaved scatter
  (golden-pinned) + pack lifetime across the async writer + all the drain paths
  must release the pack ref (RT8/L1 UAF territory — the reason the orchestrator
  flagged it). `PackIn` would also need a handle accessor (currently private).

### 20 MP measurement — DONE

20 MP@6 = 352 MB/s @ 1.56 cores (sustained); 20 MP@8 = 443 MB/s @ 1.88 cores (near
ceiling); 20 MP@30 byte-cap-drops (correct protection). Post-wave 20 MP sustains
6–8 fps vs the brief's "~4–5 fps marginal" baseline. In doc 32.

## Env notes for a fresh agent

- Build: import vcvars64 in PowerShell first (see the team-lead brief); Ninja
  Multi-Config; backend target is `xinsp_backend` (underscore); exe is
  `xinsp-backend.exe`. Plugins: rebuild `plugins/build` so `plugins/<name>/xi-*.dll`
  refresh (the loaded DLLs are the stale ones otherwise — cost me an hour).
- Bench harness: scratchpad `xr02bench/` (copied from the brief's bench5mp/20mp;
  `bench_cpu.py` = drain + backend-CPU sampler; `dumb_drain.py` = server ceiling).
- Do NOT `git checkout -- examples/` blindly — it reverts driver.py edits (it bit me).
