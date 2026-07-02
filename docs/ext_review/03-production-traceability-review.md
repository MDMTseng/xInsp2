# xInsp2 Production Truth and Traceability Review

| Field | Value |
|---|---|
| Review date | 2026-07-02 |
| Scope | Inspection identity, provenance, evidence continuity, reproducibility, audit persistence, and decision trust |
| Status | Advisory review |

Related reviews:

- [`01-project-taste-review.md`](./01-project-taste-review.md)
- [`02-core-and-developer-ux-review.md`](./02-core-and-developer-ux-review.md)

## Scope

This review asks whether xInsp2 can answer a production question after the live
process, UI, and original operator context are gone:

> What exactly judged this physical part, from which inputs and configuration,
> and can that claim be verified or reproduced?

The review covers:

- inspection and physical-part identity;
- trigger, source, and timing correlation;
- project, script, plugin, recipe, model, and calibration provenance;
- verdict completeness and failure semantics;
- evidence capture and durable persistence;
- crash, restart, drop, and disconnect continuity;
- replay and reproducibility;
- retention, integrity, and audit access.

This is not a regulatory compliance certification. The required retention,
electronic-signature, access-control, and validation rules depend on the
deployment domain. The goal is to establish a technically coherent evidence
foundation that a regulated deployment could build on.

## Executive Summary

xInsp2 has several useful traceability primitives but does not yet produce a
complete production evidence record.

Existing strengths include:

- a 128-bit trigger ID;
- process-local `run_id`;
- wall and monotonic clock separation;
- source and dispatch-group context;
- a dedicated verdict event;
- ordered sink delivery;
- plugin and script build/version information in selected diagnostics;
- crash breadcrumbs and supervisor crash history;
- transactional project working-copy commits;
- SHA-256 support;
- configurable record persistence through plugins.

These mechanisms are not joined into one immutable inspection envelope.
Currently:

- `run_result` omits the trigger ID;
- dropped events may omit `run_id`;
- `run_id` restarts with the backend process;
- part, lot, serial, recipe, project revision, script generation, plugin hashes,
  model/calibration identity, and input image hashes are absent;
- successful runs without an explicit verdict default to NA;
- inspect crashes emit NA rather than a framework failure;
- a fatal crash or watchdog process exit cannot emit the promised terminal
  result before death;
- the headless runner records frame numbers and crash count, not verdicts or
  provenance;
- `record_save` writes useful files, but not an atomic or tamper-evident journal.

The highest-leverage design is:

> The core should stamp a small immutable identity and execution envelope for
> every accepted inspection. Plugins should own evidence selection, storage,
> retention, MES delivery, and domain-specific fields.

This preserves the minimal-core principle. The core owns only facts that no
plugin can reconstruct reliably after execution: process/session identity,
inspection sequence, trigger identity, active runtime revision, execution
outcome, and timing.

## Scorecard

| Area | Score | Assessment |
|---|---:|---|
| Trigger identity | 6/10 | Strong 128-bit primitive, not carried through the verdict path |
| Run identity | 4/10 | Ordered within a process, not globally unique or restart-stable |
| Physical-part identity | 2/10 | Can ride metadata, but has no canonical contract or validation |
| Runtime provenance | 3/10 | Version/hash primitives exist but are not bound to each decision |
| Verdict completeness | 4/10 | Dedicated result exists, but crash/timeout/no-verdict semantics remain incomplete |
| Evidence persistence | 3/10 | Convenience save plugin exists; no canonical journal contract |
| Crash/restart continuity | 5/10 | Strong crash diagnostics, weak per-inspection ledger continuity |
| Replay/reproducibility | 3/10 | In-memory replay exists; deterministic evidence replay is deferred |
| Time semantics | 6/10 | Wall/monotonic separation is good; time quality and cross-device sync are absent |
| Integrity/tamper evidence | 2/10 | SHA-256 exists, but evidence is not chained, signed, or manifest-bound |
| Headless production report | 2/10 | Runner output cannot currently prove pass/fail decisions |
| Overall production traceability | 4/10 | Good primitives, no complete decision record |

## Design Guardrails

The traceability design should preserve these constraints:

1. **Do not put MES, database, retention, or compliance policy in the core.**
2. **Do not hash or serialize full images on the inspection hot path by default.**
3. **Do not make evidence storage a prerequisite for low-latency inspection.**
4. **Do not let a plugin rewrite host-stamped identity or outcome fields.**
5. **Do not claim exactly-once durable delivery without an acknowledged journal.**
6. **Do not treat a WebSocket event as an audit record.**
7. **Do not conflate operator-facing messages with stable reason codes.**
8. **Do not promise deterministic replay without capturing all external inputs.**
9. **Do not call evidence tamper-proof when it is only checksummed.**
10. **Do not make optional observability fields appear mandatory to every plugin.**

## Findings

### 1. There is no canonical inspection identity across restarts

`run_id` is an `int64` sequence allocated by the backend. It is useful for
ordering and correlation during one process lifetime, but it is not sufficient
as a durable identity:

- it restarts when the process restarts;
- a second station can generate the same value;
- a dropped event may not receive or emit a `run_id`;
- a fatal crash can terminate before the sequence is journaled;
- report consumers need station/session context to interpret it.

The trigger layer already creates a 128-bit ID when a source does not provide
one. That ID is closer to an inspection identity, but it is not emitted in the
current `run_result` payload.

#### Recommendation

Use a composite identity:

```text
inspection_id = station_id + boot_id + run_id
trigger_id    = source-provided or host-generated 128-bit ID
```

Where:

- `station_id` is stable deployment identity;
- `boot_id` is a random 128-bit value generated once per backend process;
- `run_id` is the monotonic process sequence;
- `trigger_id` correlates acquisition and inspection.

Expose a canonical string form:

```text
station-a/01J...boot/000000000123
```

Do not replace the efficient numeric `run_id` on the hot path. Add stable context
around it.

#### Acceptance Criteria

- Every accepted, dropped, completed, and failed inspection has an
  `inspection_id`.
- IDs do not collide across restarts or stations.
- `trigger_id` appears in the outcome envelope.
- A consumer can order results within one boot without parsing wall time.

### 2. Physical-part identity has no canonical contract

The trigger metadata record can carry arbitrary fields such as a barcode,
recipe, or command ID. This flexibility is useful but leaves critical questions
undefined:

- Which key is the part identity?
- Is it assigned before acquisition or inside the script?
- Can two triggers claim the same part?
- What happens when the barcode is unreadable?
- How are carrier, cavity, lane, lot, and serial represented?
- Is identity allowed to change after inspection starts?

Without a contract, each source, script, comm plugin, and MES adapter will invent
different field names and fallback behavior.

#### Recommendation

Define an optional canonical `subject` block in trigger metadata:

```json
{
  "subject": {
    "part_id": "P20260702-000184",
    "lot_id": "LOT-42",
    "carrier_id": "TRAY-19",
    "position": "C07"
  }
}
```

Rules:

- identity is captured at ingress and immutable for the run;
- missing identity is explicit, never an empty successful value;
- source-specific metadata remains allowed outside the canonical block;
- duplicate/reused IDs are policy decisions owned by a line integration plugin;
- the core copies the block into the evidence envelope without understanding its
  business meaning.

#### Acceptance Criteria

- The same subject identity reaches result, evidence storage, and MES output.
- Missing or unreadable identity has a stable reason code.
- Scripts cannot silently change host-stamped subject identity.

### 3. `run_result` is a live event, not a production evidence envelope

The current result contains:

- code;
- human message;
- optional `run_id`;
- optional duration;
- source;
- group.

The design document already lists part ID, recipe, program version, defects, and
schema as future work. The implementation also omits the recommended trigger ID.

A human message is not a stable analytical contract. Text changes, localization,
or extra diagnostics should not change defect classification.

#### Recommendation

Version the outcome schema and separate machine and human meaning:

```json
{
  "schema": "xi.run-outcome/1",
  "inspection_id": "station-a/boot-id/123",
  "run_id": 123,
  "trigger_id": "7f...:9a...",
  "subject": {
    "part_id": "P20260702-000184"
  },
  "outcome": {
    "class": "ok",
    "code": 1,
    "reason_code": "within_limit",
    "message": "Object count within limit"
  },
  "timing": {
    "captured_at_us": 1782,
    "started_at_us": 1830,
    "finished_at_us": 2940,
    "queue_us": 48,
    "inspect_us": 1110
  },
  "runtime": {
    "station_id": "station-a",
    "boot_id": "...",
    "project_revision": "...",
    "script_revision": "...",
    "config_revision": "..."
  }
}
```

The live WS event may carry a compact subset plus an envelope reference. A
journal sink receives the full form.

#### Acceptance Criteria

- Outcome consumers do not parse `message` for classification.
- Schema version is explicit.
- Host fields cannot be overwritten by script metadata.
- The envelope supports extension without changing existing field meaning.

### 4. The “one result per trigger” invariant is not currently true

The design goal says every trigger produces exactly one result, including drops,
crashes, and timeouts. Current behavior has important exceptions:

- dropped events emit a framework result but may omit `run_id`;
- script completion without `xi::result()` becomes NA, not
  `XI_SYS_NO_VERDICT`;
- an inspect exception emits code `0` with `"inspect error"`, not
  `XI_SYS_CRASHED`;
- watchdog hard trips terminate the backend process;
- an unhandled plugin or script fault can kill the backend before a terminal
  result is emitted;
- stop-boundary ordering intentionally relaxes;
- a process death can leave accepted triggers with no durable terminal outcome.

This is not just missing metadata. It means a consumer cannot distinguish:

- no part arrived;
- a result was lost;
- the process died;
- a frame was accepted but never judged;
- a run intentionally produced NA.

#### Recommendation

Use a two-stage durable lifecycle:

```text
accepted -> terminal
```

The ingress path issues an accepted record:

```json
{
  "inspection_id": "...",
  "trigger_id": "...",
  "state": "accepted",
  "accepted_at_us": 123
}
```

The normal runtime later issues exactly one terminal state:

```text
ok | ng | na | dropped | crashed | timeout | cancelled | abandoned
```

After a backend crash, the supervisor or journal reconciler closes any accepted
records without terminal outcomes as `abandoned` or `process_crashed`.

This is the only credible way to preserve continuity across a process death. A
dying process cannot guarantee that it emits its own final event.

#### Acceptance Criteria

- Every accepted ID eventually has one durable terminal state.
- Reconciliation is idempotent.
- Duplicate terminal delivery does not create duplicate decisions.
- NA, no-verdict, crash, timeout, cancellation, and drop remain distinct.

### 5. Runtime provenance is not bound to each decision

xInsp2 can report its version and commit. Crash reports record build identity.
Plugin certification code can hash DLLs. The runtime knows script DLL paths,
plugin locations, project files, instance configs, and state schema versions.

These facts are not captured together at the moment of inspection. A result
cannot currently prove:

- which script binary executed;
- which plugin binaries participated;
- which instance definitions were active;
- which working-copy revision was running;
- whether the active script was last-good after a failed compile;
- which config group commit applied;
- which optional accelerator/backend influenced execution.

#### Recommendation

Create a `RuntimeRevision` snapshot when executable/config state changes, not on
every frame:

```json
{
  "revision_id": "sha256:...",
  "xinsp_version": "0.2.0",
  "xinsp_commit": "abc123",
  "project_manifest_hash": "...",
  "script": {
    "source_hash": "...",
    "binary_hash": "...",
    "generation": 42,
    "state_schema": 2
  },
  "plugins": [
    {
      "instance": "det0",
      "plugin": "blob_analysis",
      "binary_hash": "...",
      "config_hash": "...",
      "abi": 11
    }
  ],
  "toolchain": {
    "compiler": "...",
    "opencv": "...",
    "accelerators": ["turbojpeg"]
  }
}
```

Each inspection envelope carries only `runtime_revision_id`. The full revision
manifest is emitted once to the evidence journal and cached by consumers.

This avoids per-frame hashing and serialization.

#### Acceptance Criteria

- Every result resolves to one immutable runtime revision.
- A failed compile does not change the active revision.
- Config commit produces a new revision only after successful activation.
- Revision hashing is deterministic and excludes transient paths/timestamps.

### 6. Project save identity and active runtime identity are different facts

The working-copy mechanism protects edits and transactional commit to the
canonical project. It does not define a production revision identity.

At runtime there may be:

- canonical project files;
- an active `.xinsp_work` copy;
- unsaved edits;
- a last-good compiled script from an earlier source version;
- live-tuned parameters not yet committed;
- plugin state restored from instance definitions;
- a staged config awaiting `commit_group`.

Calling all of these “the project” is insufficient for traceability.

#### Recommendation

Name and expose distinct revisions:

| Revision | Meaning |
|---|---|
| `canonical_revision` | Hash of committed canonical project content |
| `working_revision` | Hash of current working-copy content |
| `active_code_revision` | Script/plugin binaries currently executing |
| `active_config_revision` | Instance definitions and params currently active |
| `pending_config_revision` | Prepared but not committed configuration |

The inspection envelope binds only active code and active config. UI can show
whether working/canonical content differs.

#### Acceptance Criteria

- “Saved,” “compiled,” and “active” are never represented by one ambiguous flag.
- Evidence never claims the editor's current source judged a part when last-good
  code remained active.
- A config commit has a single activation boundary.

### 7. Script generation is observable indirectly but not part of results

Compile status uses a retained status sequence and the runtime keeps the
last-good script after a failed compile. This is good operational behavior.

However, run results do not include an active script generation or revision. A
developer or operator can observe new source in the editor while results still
come from the previous loaded DLL.

#### Recommendation

Add:

- monotonically increasing script generation;
- source hash;
- loaded DLL hash;
- compile attempt ID;
- active generation in every runtime revision;
- compile attempt and activation events as separate facts.

#### Acceptance Criteria

- Every result identifies the active generation.
- Compile failure records attempted source hash without changing active hash.
- State migration outcome is attached to the activation event.

### 8. Plugin participation is not represented

A project revision can list configured plugin instances, but an individual run
may call only a subset. Dynamic script branches can call different instances
for different parts.

Graph capture can observe topology, and `Record` provenance can identify
producers, but no durable per-run participation list is attached to the result.

#### Recommendation

Offer two evidence levels:

1. **Configured provenance:** all active plugin instances and binary/config
   hashes in `RuntimeRevision`.
2. **Executed provenance:** optional compact list or digest of instances actually
   called during the run.

Executed provenance should be opt-in when its hot-path cost matters. A low-cost
implementation can update a per-run fixed/pooled collector at the existing
host-to-plugin call boundary.

#### Acceptance Criteria

- Runtime revision always identifies configured dependencies.
- High-assurance deployments can prove which plugin instances executed.
- Default deployments do not pay unbounded per-run allocation cost.

### 9. Input evidence is neither identified nor reproducibly retained

The trigger carries images and metadata through the live run. After release,
those pooled buffers may no longer exist. The result does not contain:

- image content hash;
- camera sequence/frame ID;
- acquisition settings;
- sensor timestamp;
- calibration identity;
- evidence retention decision.

`expose` produces JPEG for viewing. JPEG is lossy and subscriber-dependent, so it
cannot be treated as canonical inspection input evidence.

#### Recommendation

Separate three concepts:

| Concept | Purpose |
|---|---|
| Input identity | Hash/metadata proving which frame was judged |
| Diagnostic preview | Lossy JPEG for humans |
| Retained evidence | Policy-selected original or encoded artifact |

Add an optional source-provided acquisition block:

```json
{
  "acquisition": {
    "camera_id": "cam-left",
    "frame_id": 918442,
    "sensor_ts_ns": 123456789,
    "format": "mono8",
    "width": 2048,
    "height": 1536,
    "calibration_revision": "sha256:..."
  }
}
```

Image hashing should be policy-driven:

- no hash for lowest-latency deployments;
- sampled or NG-only hashing;
- asynchronous hashing before buffer release;
- source-provided hardware/frame checksum where trustworthy;
- full original retention only where required.

#### Acceptance Criteria

- Preview artifacts are never presented as original evidence.
- Evidence policy is explicit per deployment.
- An evidence record states whether original input was retained, hashed only, or
  not retained.

### 10. Time is correctly separated by intent, but time quality is unknown

The core distinguishes wall clock for timestamps and monotonic clock for
durations/deadlines. This is good engineering.

Traceability needs additional facts:

- timezone/UTC representation;
- clock source;
- synchronization state;
- offset/uncertainty;
- camera clock versus host clock;
- behavior during NTP correction;
- station-to-station comparability.

Wall-clock timestamps may jump. A timestamp alone cannot prove ordering.

#### Recommendation

Each envelope should include:

- UTC epoch timestamp;
- monotonic sequence/order fields;
- boot ID;
- optional time-quality metadata from a platform/line integration service.

For synchronized acquisition, sources should report sensor time and mapping to
host time rather than pretending all timestamps share an epoch.

#### Acceptance Criteria

- Ordering never depends exclusively on wall time.
- Stored timestamps are unambiguously UTC.
- Clock corrections do not create duplicate inspection IDs.
- Deployments requiring synchronized stations can report time quality.

### 11. `record_save` is not an audit journal

The shipped save plugin:

- creates an output directory;
- generates a filename from count/timestamp;
- writes pretty JSON;
- writes BMP images;
- returns a saved flag.

It is useful as an example and convenience sink. It lacks audit-journal
properties:

- no atomic multi-file commit;
- no fsync/durability acknowledgement;
- no collision-proof identity across restart;
- no schema/version envelope;
- no runtime provenance;
- no content manifest;
- no checksum chain;
- no partial-write recovery;
- no retention or disk-pressure policy;
- no delivery queue/backpressure contract;
- no distinction between requested and durably committed;
- image keys are used in filenames without a documented canonical sanitizer.

#### Recommendation

Do not evolve `record_save` silently into a compliance system. Introduce a
separate reference `evidence_journal` plugin with an explicit contract:

```text
append(envelope, artifacts)
-> accepted
-> durably_committed(sequence, manifest_hash)
```

Suggested on-disk shape:

```text
journal/
  segments/
    000001.xij
  objects/
    sha256/ab/cd...
  manifests/
    runtime-revision-id.json
  checkpoint.json
```

Properties:

- append-only journal;
- length and checksum per record;
- crash-safe segment recovery;
- content-addressed artifact storage;
- idempotency by inspection ID;
- explicit durable acknowledgement;
- bounded queue and declared overflow policy;
- retention policy outside the core;
- optional hash chaining/signing.

#### Acceptance Criteria

- Power loss cannot produce a valid-looking partial record.
- Replaying an append does not duplicate an inspection.
- Disk-full behavior is explicit and visible to health.
- “Saved” means durably committed according to documented guarantees.

### 12. Hashing capability exists but no evidence integrity model uses it

SHA-256 is already implemented for authentication and plugin content checks.
This is sufficient to build content identity, but a collection of hashes does
not establish audit integrity by itself.

Threats include:

- deletion of an NG record;
- replacement of an evidence file and its adjacent hash;
- insertion/reordering of records;
- modification by a privileged local user;
- loss of an entire journal segment.

#### Recommendation

Define integrity levels:

| Level | Guarantee |
|---|---|
| L0 | No integrity claim |
| L1 | Content checksums detect accidental corruption |
| L2 | Hash-chained journal detects record modification/reordering |
| L3 | Signed checkpoints provide external tamper evidence |
| L4 | Remote/WORM acknowledgement protects against local deletion |

Deployments select a level. Documentation must use precise terms:

- checksum;
- tamper-evident;
- signed;
- immutable/WORM.

Do not call L1 tamper-proof.

### 13. Crash diagnostics and inspection evidence are separate timelines

Crash reports contain:

- process/build identity;
- faulting module;
- thread breadcrumbs;
- last run ID;
- current instance/plugin/phase.

This is strong forensic data. It is not linked to a durable accepted-inspection
record, and the crash filename/timestamp is not a canonical evidence ID.

#### Recommendation

Add `boot_id` and `inspection_id` to crash reports and FE crash history. The
evidence journal/reconciler can then:

1. locate open accepted inspections for the crashed boot;
2. mark them terminal as process-crashed/abandoned;
3. attach the crash-report reference;
4. resume with a new boot ID.

#### Acceptance Criteria

- An operator can navigate from a missing result to the responsible crash.
- A crash report cannot be mistaken for the next backend boot.
- Reconciliation does not alter already-terminal records.

### 14. The headless runner does not report inspection verdicts

`xinsp-runner` describes itself as the smallest path from saved project to a
pass/fail log. The current report contains:

- project path;
- frame numbers that ran;
- requested frame count;
- crash count;
- total duration.

It does not wire or capture the script result callback. A cleanly executed NG,
NA, or missing-verdict frame is indistinguishable in the report. The runner also
compiles in a shared temporary output directory and does not stamp source/binary
hashes or plugin/config revisions.

Its exit code indicates crashes or build/load failures, not inspection verdict.

This is the clearest mismatch between product claim and production truth.

#### Recommendation

Make the runner consume the same outcome/evidence contract as the backend:

- install result callback;
- assign boot and inspection IDs;
- capture trigger/input identity;
- load a runtime revision manifest;
- produce one terminal envelope per frame;
- write report atomically;
- define exit policy separately from per-part verdict.

Suggested summary:

```json
{
  "schema": "xi.runner-report/1",
  "runtime_revision_id": "...",
  "counts": {
    "ok": 995,
    "ng": 3,
    "na": 0,
    "system": 2
  },
  "evidence": [
    { "inspection_id": "...", "outcome": { "class": "ok", "code": 1 } }
  ]
}
```

#### Acceptance Criteria

- An NG frame appears as NG without causing an infrastructure exit failure.
- A crash appears as a system outcome and affects process exit policy.
- Runner and backend assign the same outcome semantics.
- Report write is crash-safe and schema-versioned.

### 15. Replay is not yet a reproducibility guarantee

In-memory buffer replay helps tuning and workflow. Deterministic production
replay requires more:

- original input bytes or a trustworthy content reference;
- original trigger and subject metadata;
- active runtime revision;
- config/model/calibration assets;
- ordering and timing policy;
- random seeds;
- external service responses;
- environment/accelerator differences;
- explicit comparison tolerance for nondeterministic algorithms.

The current roadmap correctly defers on-disk deterministic replay. It should not
be implied by generic record/replay language.

#### Recommendation

Define replay levels:

| Level | Meaning |
|---|---|
| R0 | Re-run a recent in-memory frame for tuning |
| R1 | Re-run retained input against current runtime |
| R2 | Reconstruct original runtime revision and compare |
| R3 | Bitwise deterministic replay under a pinned environment |

Most production investigations need R2, not necessarily R3.

#### Acceptance Criteria

- Every replay operation states its level.
- Comparison distinguishes code/config drift from algorithm nondeterminism.
- Current-runtime replay is not presented as reproduction of the original
  decision.

### 16. Evidence retention and overload policy are undefined

Evidence can be much larger and slower than inspection:

- original images may be megabytes;
- NG bursts can overwhelm storage;
- network/MES delivery can stall;
- disk can fill;
- retention rules vary by outcome and lot;
- privacy or customer data may appear in images/metadata.

The core already has explicit queue and overflow policies for inspection. The
evidence path needs equally explicit policy, but it belongs in an evidence
plugin.

#### Recommendation

Require evidence plugins to declare:

- queue capacity;
- overflow behavior;
- durability level;
- retention policy;
- artifact selection policy;
- disk low/full thresholds;
- health/status reporting;
- whether inspection may continue when evidence cannot be committed.

Example policies:

```text
all results + NG images
sample 1% OK images
retain 30 days
stop line if verdict journal cannot commit
continue with alarm if optional preview storage fails
```

#### Acceptance Criteria

- Mandatory verdict evidence and optional diagnostic images have separate
  failure policies.
- Disk pressure becomes a persistent health fault before writes fail.
- Evidence drops are themselves counted and journaled.

### 17. Access, correction, and audit actions are not modeled

Production evidence is rarely write-only. Systems need to:

- query by part/lot/time/reason/runtime revision;
- export a case;
- attach an operator disposition;
- mark rework or false reject;
- preserve the original machine decision;
- record who changed or acknowledged what.

Overwriting the original result destroys traceability.

#### Recommendation

Use append-only annotations:

```json
{
  "type": "disposition",
  "inspection_id": "...",
  "actor": "operator-17",
  "at": "...",
  "value": "accepted_after_review",
  "reason": "fixture contamination"
}
```

Machine outcome, human disposition, and downstream MES state remain separate
facts.

#### Acceptance Criteria

- Original machine verdict is immutable.
- Corrections and acknowledgements identify actor, time, and reason.
- Query/export preserves the complete event history.

## Proposed Evidence Model

### Core-owned facts

Only facts that the runtime uniquely knows should be core-stamped:

- schema version;
- station ID;
- boot ID;
- process-local run ID;
- inspection ID;
- trigger ID;
- accepted/terminal lifecycle;
- source/group;
- active runtime revision ID;
- outcome class/code/reason;
- host timing;
- system failure classification.

### Source-owned facts

- physical subject identity;
- acquisition identity;
- sensor timestamp;
- camera/frame sequence;
- source metadata;
- calibration reference supplied by acquisition.

### Script/plugin-owned facts

- defect/features;
- measurements;
- domain reason codes;
- regions/overlays;
- optional executed-plugin provenance;
- model-specific evidence.

### Evidence-plugin-owned facts

- durable journal sequence;
- persistence timestamp;
- artifact content hashes;
- storage location;
- retention class;
- delivery acknowledgements;
- hash-chain/checkpoint/signature data.

### Proposed envelope

```json
{
  "schema": "xi.inspection-evidence/1",
  "identity": {
    "station_id": "station-a",
    "boot_id": "019...",
    "run_id": 123,
    "inspection_id": "station-a/019.../123",
    "trigger_id": "001122...:aabbcc..."
  },
  "subject": {
    "part_id": "P20260702-000184",
    "lot_id": "LOT-42"
  },
  "lifecycle": {
    "state": "terminal",
    "accepted_at_us": 1782000000,
    "terminal_at_us": 1782001210
  },
  "outcome": {
    "class": "ng",
    "code": -2,
    "reason_code": "edge_chip",
    "message": "Edge chip above limit"
  },
  "timing": {
    "capture_wall_us": 1782000000,
    "queue_us": 50,
    "inspect_us": 1160
  },
  "runtime": {
    "revision_id": "sha256:...",
    "script_generation": 42
  },
  "acquisition": {
    "camera_id": "cam0",
    "frame_id": 918442,
    "calibration_revision": "sha256:..."
  },
  "details": {
    "measurements": {
      "chip_mm": 0.42
    },
    "defects": [
      {
        "code": "edge_chip",
        "region": [120, 80, 24, 18]
      }
    ]
  },
  "artifacts": [
    {
      "role": "input",
      "key": "frame",
      "retention": "original",
      "sha256": "...",
      "content_type": "image/x-xi-raw"
    }
  ],
  "journal": {
    "sequence": 88219,
    "manifest_hash": "sha256:..."
  }
}
```

## Truth Invariants

The implementation and tests should enforce:

1. Every accepted inspection has one globally unique inspection ID.
2. Every accepted inspection reaches exactly one durable terminal state.
3. A terminal outcome cannot be changed; later human actions are annotations.
4. Every outcome identifies the active runtime revision.
5. Active revision changes only at successful code/config activation boundaries.
6. Trigger and subject identity cannot be rewritten by script output.
7. Live UI events are views of evidence, not evidence storage.
8. Preview images are not original evidence.
9. A crash can leave open accepted records, but reconciliation must close them.
10. Storage failure is explicit and reflected in health.
11. Hashes identify content; signatures/checkpoints establish stronger integrity.
12. Replay states whether it uses original or current runtime.

## Prioritized Roadmap

### P0: Make Current Claims Accurate

1. Stop claiming that the runner produces a pass/fail log until it records
   verdicts.
2. Document the real exceptions to “one result per trigger.”
3. Emit trigger ID and a boot/session ID in `run_result`.
4. Distinguish inspect error from NA.
5. Define stable outcome class and reason code separately from message.
6. Version the result schema.

### P1: Establish Identity and Runtime Revision

1. Add station ID and boot ID.
2. Define canonical inspection ID.
3. Add subject metadata contract.
4. Build runtime revision manifests at activation boundaries.
5. Include active script generation/revision in outcomes.
6. Bind config commit success to a new active config revision.

### P2: Establish Durable Lifecycle

1. Define accepted and terminal records.
2. Implement a reference evidence journal plugin.
3. Make appends idempotent by inspection ID.
4. Add supervisor reconciliation after process death.
5. Link crash reports to boot and inspection IDs.
6. Surface evidence-journal health and disk pressure.

### P3: Make Headless Production Honest

1. Wire result callbacks into `xinsp-runner`.
2. Use the same evidence envelope as the backend.
3. Produce atomic, schema-versioned reports.
4. Include runtime revision manifest.
5. Separate infrastructure exit status from part verdict counts.
6. Add golden tests comparing backend and runner semantics.

### P4: Add Configurable Evidence Depth

1. Add input/acquisition identity.
2. Add optional image hashes and content-addressed artifacts.
3. Add configured and executed plugin provenance levels.
4. Define evidence retention/overflow policies.
5. Add integrity levels and signed checkpoints.
6. Add append-only human disposition events.

### P5: Reproducibility

1. Define replay levels R0-R3.
2. Capture runtime revision and retained input references.
3. Add R2 reconstruction and comparison tooling.
4. Document nondeterministic sources and tolerance policy.
5. Add evidence bundle export for incident investigation.

## Suggested First Implementation Slice

The first slice should avoid storage and image hashing. It can deliver immediate
value with low hot-path cost:

1. Generate `boot_id` at backend startup.
2. Add configured `station_id`.
3. Format `inspection_id = station_id/boot_id/run_id`.
4. Carry trigger ID into `RunOutcome`.
5. Add `schema`, `inspection_id`, `boot_id`, `trigger_id`, `outcome.class`, and
   `reason_code` to `run_result`.
6. Add active script generation and xInsp build identity.
7. Emit `XI_SYS_CRASHED` for caught inspect errors instead of NA.
8. Wire the same callback and envelope into the runner.
9. Add protocol tests for success, NG, NA, drop, caught crash, restart uniqueness,
   and last-good compile generation.

This slice creates truthful identity and outcome semantics before designing a
durable journal.

## Decision Checklist

### Identity

- Is the ID unique across process restarts and stations?
- Can acquisition, inspection, result, crash, and MES records use the same ID?
- Is missing subject identity explicit?

### Provenance

- Which exact code and config revision judged the part?
- Does a failed compile leave the active revision unchanged?
- Are model and calibration assets identified?
- Are runtime hashes computed at activation rather than per frame?

### Outcome

- Are OK, NG, NA, system failure, drop, timeout, and no-verdict distinct?
- Is classification independent of human message text?
- Can a terminal outcome be overwritten?

### Persistence

- Is acknowledgement memory-accepted or durably committed?
- What happens on disk full, power loss, or duplicate delivery?
- Is the journal append-only and recoverable?
- Are retention and evidence-drop policies explicit?

### Integrity

- Is the claim checksum, tamper-evident, signed, or immutable?
- Can record deletion or reordering be detected?
- Are checkpoints stored outside the local writable machine when required?

### Reproducibility

- Is original input retained or only previewed?
- Can the original runtime revision be reconstructed?
- Are external inputs and random/nondeterministic behavior accounted for?
- Is the replay level stated?

### Privacy and Access

- Can images contain personal or customer-sensitive data?
- Who can query, export, annotate, or delete evidence?
- Is operator disposition separate from machine verdict?

## Success Metrics

- Percentage of terminal outcomes with complete inspection identity.
- Percentage of outcomes resolving to a runtime revision manifest.
- Number of accepted inspections without terminal reconciliation.
- Evidence journal duplicate and corruption recovery test pass rate.
- Runner/backend outcome semantic parity.
- Time required to answer “what judged this part?”
- Percentage of NG cases reproducible at R2 level.
- Evidence loss count by policy and cause.
- Disk-full warning lead time.
- Number of production incidents requiring message-text parsing.

## Final Judgment

xInsp2 has enough low-level identity, timing, hashing, lifecycle, and plugin
primitives to build strong production traceability without turning the core into
an MES or database.

The missing piece is a coherent truth model.

The current result event answers “what did the live script say?” It does not yet
answer “what exact system state judged this exact physical part, and where is the
durable evidence?”

The correct boundary is:

- core stamps immutable identity, active revision, timing, and execution outcome;
- sources supply physical and acquisition identity;
- scripts/plugins supply domain evidence;
- an evidence plugin owns durable journaling, retention, delivery, and integrity.

Implement identity and outcome truth first. Add storage, signatures, and replay
only after those invariants are stable.
