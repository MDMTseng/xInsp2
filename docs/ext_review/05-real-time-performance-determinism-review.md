# xInsp2 Real-Time Performance and Determinism Review

| Field | Value |
|---|---|
| Review date | 2026-07-02 |
| Scope | Performance contracts, benchmark validity, latency tails, overload, scheduling, observation cost, long-run stability, and reproducibility |
| Status | Advisory review |

Related reviews:

- [`02-core-and-developer-ux-review.md`](./02-core-and-developer-ux-review.md)
- [`03-production-traceability-review.md`](./03-production-traceability-review.md)
- [`04-recipe-configuration-integrity-review.md`](./04-recipe-configuration-integrity-review.md)

## Scope

This review asks two related but different questions:

1. **Performance:** Can xInsp2 demonstrate that a configured inspection meets its
   throughput and latency budget under realistic sustained load?
2. **Determinism:** Can xInsp2 explain which ordering, timing, and numerical
   properties are guaranteed, best-effort, or explicitly variable?

The review covers:

- benchmark methodology and regression gates;
- build/runtime environment identity;
- end-to-end latency measurement;
- dispatch groups, queues, drops, and overload;
- scheduler priority, affinity, and oversubscription;
- OpenMP, `xi::async`, and plugin-created threads;
- output ordering and head-of-line blocking;
- HMI/observer cost;
- memory allocation and long-run stability;
- debug versus optimized builds;
- numerical and execution reproducibility;
- performance acceptance and production monitoring.

This is not a claim that Windows provides hard real-time execution. A
general-purpose Windows process can improve priority and affinity, but it cannot
guarantee a strict worst-case scheduling deadline in the same sense as a
validated RTOS or dedicated hardware path.

## Executive Summary

xInsp2 has a stronger performance foundation than most projects at this stage:

- speed is an explicit architectural goal;
- image and document handles avoid unnecessary cross-plugin copies;
- dispatch groups own separate worker lanes;
- queue depth and overflow policies are configurable;
- drops and high-water marks are observable;
- result ordering is selectable;
- process/thread priority and CPU affinity are exposed;
- OpenMP is opt-in and can be capped;
- inspect latency has a process-wide histogram;
- microbenchmarks are integrated into CTest gates;
- AOT and production autostart compile scripts with optimization;
- stress tests exercise parallel dispatch and affinity.

The evidence does not yet justify a strong real-time or deterministic claim.

The main gaps are:

- no deployment-level performance contract;
- perf gates use best-of minimum timing rather than distribution/tail behavior;
- committed baselines contain no hardware, OS, power, compiler, or backend
  identity;
- JPEG gates compare different encoder implementations as if they were the same
  workload;
- one benchmark measures a serialization path that is no longer the common
  in-process path;
- runtime metrics measure script compute only, not end-to-end decision latency;
- queue wait, output-gate wait, ordered sink time, JPEG encode, and delivery time
  are missing;
- subscribing to observational output can add unmeasured work inside the ordered
  emission path;
- dispatch lanes isolate worker slots but not memory bandwidth, shared plugin
  locks, OpenCV/OpenMP pools, `std::async` threads, disk, or network;
- nested parallelism can oversubscribe the machine;
- `xi::async` currently uses `std::async(launch::async)`, making thread creation
  and scheduling part of per-run latency;
- timer loops use `sleep_for`, providing cadence rather than deadline-accurate
  periodic scheduling;
- allocation/memset still occurs on image creation despite stronger “no
  allocation per frame” language in project principles;
- long-duration memory, thermal, power, and degradation evidence is limited;
- build mode can change runtime behavior substantially while the active mode is
  not bound to each performance result;
- numerical determinism across OpenCV, IPP, turbojpeg, CPU, thread count, and
  floating-point reductions is not defined.

The core recommendation is:

> Define performance as a versioned deployment contract measured at explicit
> stage boundaries. Use microbenchmarks for local regression, scenario tests for
> latency distributions, and soak tests for sustained stability.

## Scorecard

| Area | Score | Assessment |
|---|---:|---|
| Performance intent | 8/10 | Speed-first is explicit and influences architecture |
| Microbenchmark coverage | 6/10 | Useful gates, limited methodology and environment identity |
| End-to-end latency visibility | 3/10 | Inspect compute measured; queue/output/delivery largely invisible |
| Overload behavior | 7/10 | Queue/drop policies and counters are unusually explicit |
| Scheduling controls | 6/10 | Priority/affinity/groups exist, guarantees are overstated |
| Parallelism control | 4/10 | Dispatch is bounded; nested OpenMP/async/plugin threads are not globally budgeted |
| Observer isolation | 3/10 | Optional work can enter ordered emission without complete cost accounting |
| Memory stability | 5/10 | Ownership counters exist; sustained allocation/working-set evidence is incomplete |
| Build-mode clarity | 5/10 | Production optimized, development behavior differs and is weakly surfaced |
| Numerical determinism | 3/10 | No declared cross-backend equivalence or tolerance policy |
| Reproducible benchmarking | 3/10 | Baselines omit environment fingerprint |
| Production performance proof | 4/10 | Strong mechanisms, incomplete evidence hierarchy |

## Terminology

Use precise performance terms:

| Term | Meaning |
|---|---|
| Throughput | Completed inspections per unit time |
| Service time | Time actively executing one stage |
| Queue wait | Time accepted work waits before service |
| Decision latency | Capture/acceptance to verdict availability |
| Delivery latency | Verdict creation to external consumer acknowledgement |
| Jitter | Variation in latency/cadence |
| Deadline miss | Decision completed after configured deadline |
| Drop | Accepted or offered work deliberately discarded by policy |
| Best effort | No bounded worst-case guarantee |
| Soft real-time | Deadline misses are measured and controlled but possible |
| Hard real-time | Deadline miss is prevented by a validated bounded system |
| Deterministic ordering | Defined ordering independent of worker completion |
| Numerical reproducibility | Result remains within declared tolerance |
| Bitwise reproducibility | Output bytes are identical |

xInsp2 should describe itself as performance-oriented or soft-real-time unless a
specific deployment supplies stronger external guarantees.

## Design Guardrails

1. **Do not equate average service time with end-to-end latency.**
2. **Do not use best-of timing to claim p99 or worst-case performance.**
3. **Do not compare baselines captured with different implementations.**
4. **Do not let optional observers silently change the measured critical path.**
5. **Do not call dedicated worker lanes fully isolated when they share CPU,
   memory, locks, and I/O.**
6. **Do not allow nested parallelism without a machine-wide concurrency budget.**
7. **Do not claim no per-frame allocation when the implementation allocates
   outputs per frame.**
8. **Do not use thread priority as a substitute for admission control.**
9. **Do not promise deterministic replay from ordered result emission alone.**
10. **Do not optimize a benchmark without validating output correctness.**
11. **Do not treat a single developer machine baseline as a portable threshold.**
12. **Do not hide active optimization/backend mode from production evidence.**

## Findings

### 1. There is no explicit performance contract

The project provides configuration knobs and benchmark numbers but does not
define a per-deployment contract such as:

```text
Input rate:             60 parts/s
Decision deadline:      12 ms from trigger acceptance
P99 decision latency:   <= 8 ms
Maximum queue wait:     2 ms
Allowed drops:          0 in normal load
Burst:                  120 parts/s for 500 ms
Recovery:               queue below 25% within 2 s
Evidence delivery:      <= 100 ms, asynchronous
```

Without a contract:

- queue depth cannot be judged correct;
- `max_parallel` cannot be sized rationally;
- a 25% microbenchmark threshold has no product meaning;
- watchdog duration is not tied to the production deadline;
- “fast” can mean throughput, median latency, or compile speed.

#### Recommendation

Add a project/deployment performance profile:

```json
{
  "performance": {
    "expected_rate_hz": 60,
    "burst_rate_hz": 120,
    "burst_duration_ms": 500,
    "decision_deadline_ms": 12,
    "p99_target_ms": 8,
    "max_queue_wait_ms": 2,
    "drop_policy": "fault",
    "warmup_frames": 20
  }
}
```

The runtime reports compliance against this profile. It should not silently tune
behavior from the values unless explicitly designed to do so.

#### Acceptance Criteria

- Performance tests use a declared workload and target.
- Runtime health reports deadline misses, not only crashes and drops.
- Queue/drop configuration has documented capacity rationale.

### 2. Perf gates measure the best observed run

The image-pool and JPEG gates:

- warm up;
- run several batches;
- retain the minimum/best batch;
- compare one integer to a baseline;
- fail above a 25% slower threshold.

Best-of measurement deliberately removes scheduler noise. This is useful for
detecting algorithmic throughput regressions. It systematically ignores:

- jitter;
- scheduler interference;
- page faults;
- cold-start cost;
- thermal throttling;
- background contention;
- long-tail latency.

A benchmark can pass while production p99 becomes much worse.

#### Recommendation

Keep best-of gates but name them **micro-throughput regression gates**.

Add distribution output:

```text
min
median
p95
p99
max
mean
standard deviation or MAD
```

Use different gate policies:

- microbenchmark: median regression after controlled warmup;
- scenario latency: p99 and deadline-miss count;
- soak: rolling p99, max, memory growth, and drops.

### 3. Baselines have no environment fingerprint

Committed baseline files contain only metric/value pairs. They do not identify:

- CPU model and topology;
- logical/physical core count;
- RAM speed/NUMA;
- Windows version/build;
- power plan;
- process priority;
- compiler version and flags;
- OpenCV version/build;
- IPP/turbojpeg presence;
- encoder backend;
- Debug/Release configuration;
- virtualization/CI environment;
- thermal state.

The repository already records a known JPEG baseline mismatch when the captured
machine had turbojpeg/IPP and another build used OpenCV.

#### Recommendation

Baseline files should carry an environment header:

```json
{
  "schema": "xi.perf-baseline/1",
  "fingerprint": {
    "cpu": "...",
    "logical_cores": 20,
    "os": "...",
    "power_plan": "high-performance",
    "compiler": "...",
    "build_type": "Release",
    "opencv": "...",
    "jpeg_backend": "turbojpeg",
    "xinsp_commit": "..."
  },
  "metrics": {}
}
```

Policy:

- same fingerprint class: compare and gate;
- incompatible backend/hardware class: skip with explicit reason or select a
  matching baseline;
- missing fingerprint: do not claim a valid comparison.

### 4. JPEG gate conflates implementation selection with regression

`bench_jpeg` reports the active encoder but the baseline key is only:

```text
jpeg_us_per_encode_1920x1080
```

OpenCV, IPP, turbojpeg, and stb are different implementations with materially
different throughput. A missing optional dependency appears as a performance
regression rather than a build-capability difference.

#### Recommendation

Include backend in metric identity:

```text
jpeg_turbojpeg_q85_1920x1080_us
jpeg_opencv_q85_1920x1080_us
```

Also add an explicit production requirement:

```text
required JPEG backend = turbojpeg
```

when throughput depends on it. Missing required acceleration should fail
capability validation before production starts.

### 5. The JPEG workload is not representative enough

The benchmark encodes a smooth synthetic gradient. Smooth gradients are highly
compressible and do not represent:

- sensor noise;
- high-frequency texture;
- binary masks;
- real defects;
- different channel layouts;
- grayscale cameras;
- varying output size and memory pressure.

Encoder time and output size can differ substantially with image content.

#### Recommendation

Use a small committed or generated corpus:

- smooth gradient;
- deterministic noise;
- checker/edge-heavy pattern;
- representative grayscale frame;
- representative production-like RGB frame where licensing permits.

Report both encode latency and bytes. Preserve deterministic generation seeds.

### 6. The record serialization benchmark describes a retired hot path

`bench_record.cpp` comments state that every in-process plugin call performs
serialize/parse round trips. The current architecture uses compatible yyjson
document pointers for the common in-process path, with serialization as fallback.

The gate therefore risks protecting a path that is no longer representative
while leaving the actual pointer/refcount/adoption path unmeasured.

The record benchmark is also optional: its third-party sources are fetched
separately, so a clean checkout may have no `perf_record` gate at all.

#### Recommendation

Replace or supplement it with:

- compatible in-process Record handoff;
- JSON-fallback handoff;
- copy-on-write mutation;
- nested record and image-bag handoff;
- document retain/release contention;
- representative plugin-chain end-to-end scenario.

Keep serialization comparison as an exploratory benchmark, not the primary hot
path gate.

### 7. Runtime latency metrics stop before the actual outcome path

`RunOutcome::dt_us` measures script inspection compute. Metrics record this value
before:

- ordered emit-gate wait;
- staged sink flush;
- expose JPEG compression;
- binary frame construction;
- WebSocket send;
- `run_result` emission;
- external PLC/MES acknowledgement.

The metric is accurately named inspect latency in comments, but UI and users can
interpret it as cycle or decision time.

#### Recommendation

Measure explicit stages:

```text
capture_to_accept_us
queue_wait_us
inspect_compute_us
emit_gate_wait_us
sink_flush_us
result_publish_us
consumer_ack_us
end_to_end_decision_us
```

Not every deployment needs consumer acknowledgement. The system should clearly
mark the last boundary it can observe.

#### Acceptance Criteria

- “Inspect time” and “decision latency” are separate metrics.
- Queue wait is included in deadline evaluation.
- Result/HMI throughput does not derive from compute duration.

### 8. Queue wait is available to scripts but not aggregated operationally

The worker stamps `dequeued_at_us`, and the trigger carries an arrival/capture
timestamp. A script can calculate queue wait. The metrics registry does not
aggregate queue wait per group or globally.

This hides the earliest overload signal:

- compute time may remain stable;
- queue wait grows;
- parts miss deadlines before drops begin.

#### Recommendation

Record per-group:

- accepted rate;
- dequeued rate;
- queue wait histogram;
- queue depth current/high-water;
- oldest queued age;
- deadline misses before service;
- drops by reason/policy.

Oldest queued age is often more actionable than queue length because frame rates
and workloads vary.

### 9. Queue depth is count-based, not deadline-based

`queue_depth` bounds item count. A queue of 100 frames can represent:

- 100 ms at 1 kHz;
- 3.3 seconds at 30 Hz;
- a much longer delay under heavy processing.

For live inspection, processing a very old frame may be less useful than dropping
it before compute.

#### Recommendation

Add optional age/deadline admission:

```text
max_queue_age_ms
deadline_from_capture_ms
drop_if_late_before_start
```

The resulting system outcome should distinguish:

- queue full;
- deadline already missed;
- rate limit;
- shutdown cancellation.

### 10. `block` overflow can move latency into source/plugin threads

Backpressure through blocking can be valid for offline archival or a source that
supports flow control. For a camera callback thread it can:

- block the camera SDK;
- exhaust driver buffers;
- delay multiple cameras;
- create opaque acquisition drops;
- deadlock a plugin with shared locks.

#### Recommendation

Declare source backpressure capability:

```text
block-safe
drop-capable
externally-paced
callback-must-return
```

Reject or warn when `overflow: block` is paired with a source that cannot safely
block.

### 11. Dispatch groups provide capacity partitioning, not full isolation

Each group owns workers and a queue. This prevents one group from consuming
another group's worker slots.

Groups still share:

- physical CPU cores unless affinity separates them;
- memory bandwidth and cache;
- allocator and ImagePool structures;
- process priority class;
- OpenCV and OpenMP pools;
- shared non-reentrant plugin instances;
- filesystem and network;
- ordered/output transport;
- backend control threads.

Calling groups isolated without qualification overstates the guarantee.

#### Recommendation

Describe guarantees precisely:

- **worker-capacity isolation:** yes;
- **CPU-time isolation:** partial, scheduler/affinity dependent;
- **memory-bandwidth isolation:** no;
- **shared-instance isolation:** no;
- **I/O isolation:** no;
- **failure isolation:** no, same process.

Add diagnostics for shared instances crossing priority groups because they can
create direct priority inversion.

### 12. Shared non-reentrant instances can invert priority

A low-priority group may hold the per-instance serialization gate while a
high-priority group waits. Raising the high-priority worker's OS priority does
not release the plugin lock.

This is documented as residual coupling but not structurally prevented.

#### Recommendation

At project validation:

- build group-to-instance usage from static schema/graph plus runtime capture;
- warn when a non-reentrant instance is shared across priority classes;
- recommend separate instances, reentrant implementation, or explicit priority
  inheritance inside the plugin where feasible.

Track per-instance gate wait time to make inversion visible.

### 13. Nested parallelism has no global budget

Potential concurrency includes:

- sum of dispatch-group workers;
- OpenMP threads per script invocation;
- `xi::async` tasks;
- OpenCV internal threads;
- plugin-owned workers;
- camera/source threads;
- HMI/server/control threads.

The current oversubscription warning primarily considers dispatch workers.

Example:

```text
4 dispatch workers
x 4 OpenMP threads each
+ async tasks
+ OpenCV internal pool
= more runnable threads than cores
```

This can reduce throughput and greatly increase p99 latency.

#### Recommendation

Create a machine-wide concurrency budget:

```json
{
  "cpu_budget": {
    "dispatch_workers": 4,
    "openmp_per_run": 2,
    "opencv_threads": 1,
    "async_workers": 2
  }
}
```

Validate worst-case runnable concurrency. Prefer one primary parallelism layer
per pipeline stage.

### 14. `xi::async` creates scheduling variability

`xi::async` currently uses `std::async` with `launch::async`. Each call can create
or acquire implementation-managed execution resources, with:

- thread creation/destruction cost;
- scheduler variance;
- no shared queue capacity;
- no affinity/priority integration;
- no global worker budget;
- difficult per-task instrumentation.

The header itself says a thread pool may replace it later.

#### Recommendation

Back `xi::async` with a bounded executor owned by the runtime or script:

- fixed worker count;
- explicit queue capacity;
- inherited inspection ID/owner/cancel context;
- priority/affinity policy;
- task wait/service metrics;
- no per-frame thread creation.

Keep the public Future API if desired.

### 15. OpenMP and OpenCV parallelism are not coordinated

OpenMP can be enabled and capped. This is useful, but:

- cap is per OpenMP runtime/team context;
- multiple simultaneous inspections may each create a team;
- OpenCV may also parallelize internally;
- plugin libraries may carry their own pools;
- nested parallelism/dynamic adjustment may change behavior.

#### Recommendation

Record and control:

- OpenMP max threads and dynamic/nested settings;
- OpenCV thread count;
- active dispatch concurrency;
- CPU affinity masks;
- selected acceleration backend.

Include these in runtime revision and performance reports.

### 16. Priority and affinity APIs do not prove they were applied

Thread priority and affinity calls are attempted, but return values are generally
not surfaced as persistent health. Invalid or unsupported masks can be ignored.

The Windows affinity implementation uses a `DWORD_PTR` bit mask, which may not
cover processor-group topology on machines with more than 64 logical processors.

#### Recommendation

- Check every scheduler/affinity call result.
- Publish requested versus effective settings.
- Detect Windows processor groups and NUMA topology.
- Warn when a configured core does not exist or belongs to another group.
- Record effective CPU placement in performance evidence.
- Treat `realtime` process priority as a hazardous expert mode.

### 17. Timer cadence uses relative sleeps

Synthetic source/timer loops use `sleep_for`. Relative sleep loops accumulate:

- execution-time drift;
- scheduler delay;
- coarse wakeup behavior;
- phase changes after long stalls.

They are adequate for demo cadence and source-less scripts, not precision trigger
generation.

#### Recommendation

Use monotonic absolute deadlines:

```text
next += period
sleep_until(next)
record lateness
apply overrun policy
```

Real camera/PLC triggers should remain externally timed. The runtime timer should
be documented as a soft cadence source.

### 18. Ordered output can create head-of-line blocking

Arrival ordering preserves output sequence by waiting for earlier frames.
One slow frame can delay all later results even when their compute has finished.

This is an inherent trade-off, not a bug. Current metrics do not quantify:

- gate wait;
- number of completed frames blocked behind the head;
- maximum reorder depth;
- delay added by ordered sinks.

#### Recommendation

Expose ordered-gate metrics and allow per-output policy:

- completion order for low-latency telemetry;
- arrival order for PLC/MES semantics;
- independent observation stream where reordering is acceptable.

Do not make diagnostic image delivery share a critical result gate unless
required.

### 19. Observation can alter critical-path latency

The expose plugin:

- does no JPEG work when no channel is subscribed;
- compresses and emits when subscribed;
- runs as an ordered sink;
- performs compression during staged sink flush.

Staged sink flush occurs after inspect compute timing and inside the ordered
emission phase. Therefore:

- opening an HMI/viewer can add JPEG cost;
- the cost is not in inspect latency metrics;
- arrival-ordered results can wait behind that work;
- observation changes actual result-delivery latency.

This violates the intuitive expectation that attaching a viewer is observational
only.

#### Recommendation

Separate critical and observational sinks:

```text
critical ordered sinks
-> result/PLC transaction

best-effort observation sinks
-> bounded async encoder queue
-> drop/coalesce latest under load
```

Observation must carry its own queue, drop, and freshness metrics.

#### Acceptance Criteria

- Connecting HMI does not materially change decision deadline.
- Observation overload drops/coalesces previews before critical results.
- Viewer cost is measurable.

### 20. The throughput card uses service time, not production rate

The HMI documentation already notes that throughput is derived from
`run_finished.ms`, which is inspect duration, not inter-arrival or completed-parts
rate. Fast compute can display unrealistic parts/minute even when triggers arrive
slowly.

#### Recommendation

Compute:

- offered rate from accepted trigger timestamps;
- completed rate from terminal results over a time window;
- service capacity estimate separately from actual throughput;
- utilization from service time versus worker capacity.

Label each metric accurately.

### 21. Image creation still allocates and touches memory per frame

The ImagePool benchmark documents that create/release allocates a new pixel
buffer, touches it with `memset`, then deletes it on release. The project
principles use stronger language about no allocation on the per-frame path.

The benchmark may correctly show allocation is currently cheap relative to real
vision work. The architectural claim and implementation are still inconsistent.

#### Recommendation

Choose precise wording:

- zero-copy across plugin boundaries;
- bounded/refcounted ownership;
- output allocation remains per image unless reused by algorithm/plugin;
- measured allocation cost is acceptable for target rates.

Only introduce buffer reuse when scenario evidence justifies its complexity.
Do not preserve an inaccurate “zero allocation” claim.

### 22. Memory observability is object-centric, not process-centric

ImagePool reports live handles, bytes, high-water, owners, and cumulative
creation. This is useful.

Long-run stability also needs:

- process working set/private bytes;
- virtual address space;
- heap fragmentation;
- thread count;
- handle count;
- plugin-specific non-pool memory;
- encoder caches;
- file descriptors/Windows handles;
- growth rate over time.

#### Recommendation

Add low-frequency process metrics and soak assertions:

```text
after warmup, working-set slope <= threshold
thread count stable
handle count stable
ImagePool live bytes return to steady band
no monotonic plugin cache growth
```

### 23. Long-duration and thermal behavior are underrepresented

Stress tests exercise concurrency and bursts, but the retired full-stack soak
means less evidence for:

- 24/7 memory plateau;
- thermal throttling;
- power-plan changes;
- periodic antivirus/backup interference;
- log/evidence disk growth;
- repeated recipe and code reload;
- clock synchronization events;
- camera reconnect cycles.

#### Recommendation

Create scenario soaks:

- 8-hour developer/nightly;
- 24/72-hour qualification;
- repeated start/stop/reload/config switch;
- burst and idle cycles;
- forced observer connect/disconnect;
- disk pressure and network degradation.

Store time-series summaries rather than enormous raw logs.

### 24. Development builds are not performance-equivalent

Interactive script compilation defaults to `/Od` for faster edit/compile.
Project plugin development builds can use `/Od /RTC1`. Production autostart and
runner use `/O2`.

This is a good developer-experience trade-off, but:

- tuning latency in development may not represent production;
- undefined behavior may manifest differently;
- thread timing/races can change;
- benchmark results can accidentally use the wrong mode;
- users may not know which binary generation is active.

#### Recommendation

Expose active execution profile:

```text
Development Debug
Development Optimized
Production AOT
```

Add “Run Performance Validation” that compiles the exact production profile and
records its runtime revision.

### 25. Watchdog timeout is not a deadline guarantee

The watchdog detects long-running inspect work, requests cooperative
cancellation, and may terminate the backend for supervisor recovery.

It does not:

- include queue wait;
- guarantee completion before a production deadline;
- cover output/MES delivery;
- prevent an individual late result;
- provide low-cost recovery without process restart for uncooperative code.

#### Recommendation

Separate:

- decision deadline monitoring;
- cooperative cancellation budget;
- hard watchdog process-liveness protection.

Each has different thresholds and outcomes.

### 26. Result ordering is intentionally incomplete at boundaries

Arrival ordering applies during steady-state inspected runs. Documentation notes
exceptions:

- dropped results can interleave outside the gate;
- stop wakes parked emitters and can reorder the final in-flight results;
- completion mode is intentionally out of order.

This is honest but incompatible with a blanket deterministic-stream claim.

#### Recommendation

Version and document ordering semantics:

```text
compute_order
result_order
drop_order
shutdown_order
sink_order
```

Consumers should order by inspection identity/sequence and tolerate explicitly
declared exceptional markers.

### 27. Numerical determinism is undefined

Results may vary due to:

- OpenCV versus IPP implementation;
- CPU instruction sets;
- OpenMP reduction order;
- dispatch concurrency;
- unordered container iteration;
- floating-point rounding;
- library/compiler versions;
- nondeterministic plugin algorithms;
- unseeded randomness;
- model runtime backend.

For many vision applications, bitwise equality is unnecessary. A tolerance and
classification stability contract is necessary.

#### Recommendation

Define levels:

| Level | Guarantee |
|---|---|
| D0 | No reproducibility claim |
| D1 | Same verdict on validated dataset |
| D2 | Measurements within declared tolerance |
| D3 | Bitwise-identical outputs under pinned runtime |

Each plugin can declare its level and conditions. Production validation usually
targets D1/D2.

### 28. Performance optimization lacks paired correctness evidence

A faster image, JPEG, or Record path can change:

- pixel values;
- color order;
- rounding;
- metadata order;
- image lifetime;
- defect classification.

Microbenchmarks usually check successful execution, not semantic equivalence.

#### Recommendation

Every performance gate should pair with:

- golden output/hash where bitwise equality is intended;
- tolerance comparison where numerical variation is allowed;
- verdict stability dataset for production pipelines;
- ownership/leak correctness tests.

### 29. Aggregate metrics hide group and revision changes

`MetricsRegistry` counters are process-uptime cumulative and aggregate all
frames. They do not distinguish:

- dispatch group;
- project/runtime revision;
- recipe;
- source;
- build mode;
- warmup versus steady-state;
- HMI subscribed versus unsubscribed.

A new slower revision can be hidden by a long history of fast frames.

#### Recommendation

Partition or label scenario metrics by:

- boot/session;
- runtime revision;
- group;
- source;
- observation mode.

Use bounded cardinality. Resetting or opening a new metric epoch on runtime
revision activation is preferable to unbounded labels.

### 30. Performance gates are not clearly tied to stable CI hardware

The repository refers to green CI and committed baselines, but portable
performance comparison needs either:

- dedicated stable runners;
- normalized relative tests;
- environment-class baselines;
- non-blocking trend reporting on variable shared CI.

A developer laptop should not fail because it is slower than the reference
machine while functionally correct.

#### Recommendation

Use three tiers:

1. **PR micro correctness:** benchmarks run, metrics recorded, extreme regression
   checks only.
2. **Dedicated performance runner:** stable hardware, blocking regression gates.
3. **Qualification machine:** full scenario/soak against deployment contract.

## Proposed Measurement Model

### Per-inspection timestamps

```text
t_capture       source timestamp
t_accept        host ingress
t_enqueue       queue insertion
t_dequeue       worker start
t_compute_end   script/plugin compute complete
t_gate_ready    waiting for ordered emission
t_result        result published
t_ack           critical consumer acknowledgement, optional
```

Derived:

```text
ingress_delay      = t_accept - t_capture
queue_wait         = t_dequeue - t_enqueue
compute_service    = t_compute_end - t_dequeue
emit_wait          = t_result - t_compute_end
decision_latency   = t_result - t_accept
capture_to_result  = t_result - t_capture
delivery_latency   = t_ack - t_result
```

Use monotonic clocks for local durations. Cross-device capture timestamps need
an explicit clock mapping/quality model.

### Per-group metrics

- offered, accepted, dequeued, completed, dropped rates;
- queue depth and oldest age;
- queue-wait histogram;
- compute histogram;
- decision-latency histogram;
- deadline misses;
- worker utilization;
- instance-gate wait;
- ordered-gate wait;
- observation queue/drop count.

### Environment fingerprint

- station and boot ID;
- CPU/topology/NUMA;
- OS and power policy;
- process/thread priority;
- affinity;
- compiler/build flags;
- OpenCV/IPP/turbojpeg versions;
- OpenMP/OpenCV/async thread limits;
- runtime/script/plugin/config revision;
- observation subscriptions.

## Performance Evidence Hierarchy

### Level 1: Microbenchmarks

Purpose:

- detect local algorithm/data-structure regressions;
- compare implementation alternatives.

Not evidence of:

- production p99;
- end-to-end deadline;
- sustained stability.

### Level 2: Component Scenarios

Examples:

- representative plugin chain;
- queue overload;
- ordered sink with/without viewer;
- config switch under load;
- camera burst.

Measures distributions and correctness.

### Level 3: Full-System Qualification

Runs exact production bundle, hardware, camera/PLC simulator or real devices,
evidence policy, and HMI load.

Validates deployment contract.

### Level 4: Soak and Field Monitoring

Demonstrates stability over time and detects environment drift after deployment.

## Performance Invariants

1. Metrics distinguish queue wait, compute, output wait, and delivery.
2. Performance claims name workload, percentile, duration, and environment.
3. Baselines are compared only within compatible environment classes.
4. Critical result delivery is isolated from optional observation work.
5. Overload produces explicit drops/deadline misses rather than silent latency
   growth.
6. Nested parallelism remains within a declared CPU budget.
7. Active build and acceleration modes are observable.
8. A performance optimization preserves declared correctness.
9. Runtime revision changes open a new metric epoch.
10. Long-run memory/thread/handle counts reach a stable band.
11. Determinism level and tolerance are declared per plugin/pipeline.
12. Hard watchdog behavior is not presented as deadline compliance.

## Prioritized Roadmap

### P0: Correct Claims and Metrics

1. Rename current metric to `inspect_compute_ms`.
2. Fix HMI throughput to use completed results per time window.
3. Document dispatch groups as worker-capacity isolation.
4. Correct no-allocation language.
5. Mark timer cadence as soft/best-effort.
6. Mark perf gates as best-case micro-throughput gates.

### P1: Fix Benchmark Validity

1. Add environment fingerprints.
2. Include JPEG backend in metric identity.
3. Replace stale Record hot-path benchmark.
4. Add representative image corpus.
5. Report distributions, not only best.
6. Pair performance with correctness checks.

### P2: End-to-End Runtime Visibility

1. Aggregate queue-wait histograms.
2. Measure decision latency and ordered-gate wait.
3. Add oldest queue age and deadline misses.
4. Add per-group metric epochs.
5. Record active build/accelerator profile.
6. Add low-frequency process memory/thread/handle metrics.

### P3: Isolate Critical Delivery

1. Separate critical and observational sink queues.
2. Move JPEG/viewer encoding off the ordered result path.
3. Add observation coalescing/drop policy.
4. Measure critical consumer acknowledgement where required.
5. Add shared-instance priority inversion diagnostics.

### P4: Bound Parallelism

1. Replace `std::async` backing with bounded executor.
2. Introduce machine-wide concurrency budget.
3. Set/record OpenCV and OpenMP thread policy.
4. Validate affinity and processor-group topology.
5. Add source backpressure capability declarations.
6. Add age/deadline-based admission.

### P5: Qualification and Determinism

1. Define D0-D3 determinism levels.
2. Add representative pipeline datasets and tolerance contracts.
3. Build dedicated stable performance runner.
4. Add full-system qualification scenarios.
5. Restore long-duration soak with current FE/BE architecture.
6. Store performance reports with runtime revision.

## Suggested First Implementation Slice

The first slice should improve visibility before changing scheduling:

1. Rename existing latency to `inspect_compute_ms`.
2. Stamp ingress, enqueue, dequeue, compute-end, and result-publish monotonic
   times.
3. Add per-group queue-wait and decision-latency histograms.
4. Add oldest queue age and deadline-miss count.
5. Add ordered-gate and sink-flush timing.
6. Record whether expose channels are subscribed.
7. Fix HMI completed-parts throughput.
8. Add benchmark environment fingerprint and backend-aware JPEG baseline.
9. Add a viewer-off versus viewer-on scenario test.
10. Add runtime revision/build mode to the performance report.

This provides evidence needed to decide whether executor, queue, or sink
architecture changes are actually necessary.

## Decision Checklist

### Contract

- What input rate, burst, percentile, and deadline are required?
- Is the requirement throughput, latency, or both?
- What drop/degradation policy is allowed?

### Measurement

- Which stage boundaries are measured?
- Is the metric service time or end-to-end latency?
- Are warmup, sample count, percentile, and duration stated?
- Is the active runtime revision recorded?

### Benchmark

- Is the workload representative?
- Is the environment compatible with the baseline?
- Does best-of hide the property being claimed?
- Is correctness validated alongside speed?

### Scheduling

- What is the total runnable thread budget?
- Are dispatch, OpenMP, OpenCV, async, and plugin pools coordinated?
- Can a shared instance invert priority?
- Were requested priority/affinity settings actually applied?

### Overload

- What happens when queue depth or deadline is exceeded?
- Is queue age visible?
- Can the source safely block?
- Are drops and deadline misses distinct?

### Observation

- Does connecting a viewer change critical latency?
- Is encoding asynchronous and bounded?
- What gets dropped first under observation overload?

### Stability

- Does memory reach a plateau?
- Are thread and handle counts stable?
- Has the exact production profile completed a soak?
- Are thermal and power conditions controlled?

### Determinism

- Is ordering, verdict stability, numerical tolerance, or bitwise equality
  required?
- Are random seeds and backend implementations pinned?
- Does concurrency alter observable results?

## Success Metrics

- P50/P95/P99/max decision latency by runtime revision and group.
- Deadline misses and drops per million accepted inspections.
- Queue-wait share of decision latency.
- Viewer-on versus viewer-off latency delta.
- Ordered-gate wait and head-of-line depth.
- CPU oversubscription ratio.
- Effective versus requested affinity/priority match rate.
- Working-set, thread-count, and handle-count slope during soak.
- Performance-gate false failures caused by environment mismatch.
- Pipelines with declared D1/D2 validation contracts.
- Time required to reproduce a production performance report.

## Final Judgment

xInsp2 has made several correct low-level performance choices and, importantly,
has explicit overload policies rather than pretending queues are infinite.

The main weakness is not lack of optimization. It is incomplete measurement
boundaries and evidence quality.

Current benchmarks are useful for local throughput regression. Current runtime
metrics are useful for script compute health. Neither yet proves end-to-end
production latency, sustained behavior, or deterministic results.

The correct next step is not immediate scheduler or memory-pool redesign. It is:

1. define the deployment performance contract;
2. measure the complete path;
3. fingerprint the environment;
4. isolate optional observation cost;
5. then optimize the stage that scenario evidence identifies.

This keeps the speed-first principle empirical rather than aspirational.
