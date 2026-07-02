# xInsp2 Recipe and Configuration Integrity Review

| Field | Value |
|---|---|
| Review date | 2026-07-02 |
| Scope | Configuration identity, validation, atomic activation, persistence, approval, rollback, audit history, and runtime binding |
| Status | Advisory review |

Related reviews:

- [`02-core-and-developer-ux-review.md`](./02-core-and-developer-ux-review.md)
- [`03-production-traceability-review.md`](./03-production-traceability-review.md)

## Scope

This review asks:

> When xInsp2 says a recipe or configuration is active, can the system prove
> exactly which values and assets are active across every participating
> instance, and can it prevent or recover from partial activation?

The review covers:

- script parameters;
- backend-managed and script-owned instance definitions;
- plugin resource folders and heavy assets;
- `set_param` and `set_instance_def`;
- `prepare_instance` and `commit_group`;
- project save/load and working-copy commit;
- validation and failure handling;
- config revision identity;
- draft, approved, active, and retired lifecycle;
- rollback and crash recovery;
- operator change history;
- binding active configuration to inspection evidence.

The review distinguishes three different concerns that should not be collapsed:

1. **Editing transaction:** whether files are saved without corruption.
2. **Activation transaction:** whether the running line changes config as one
   coherent unit.
3. **Governance transaction:** whether an approved revision is activated by an
   authorized actor with an auditable history.

xInsp2 has strong work on the first concern and useful primitives for the
second. It does not yet provide a complete second or third concern.

## Executive Summary

xInsp2 contains several robust configuration mechanisms:

- atomic per-file JSON writes;
- a crash-recoverable working-copy mirror;
- config validation against plugin manifests;
- safe default serialization for non-reentrant plugins;
- background `prepare` for heavy resources;
- a dispatch drain barrier around `commit_group`;
- last-good script/plugin behavior after compile failures;
- warnings when project or recipe fields fail to apply;
- state-schema migration for script runtime state.

These mechanisms do not yet form an atomic recipe system.

The most important findings are:

- `load_project` applies parameters and instance definitions sequentially while
  continuous inspection may still run;
- successful fields remain active when later fields fail;
- the command returns `ok: true` with warnings for partial application;
- `prepare_instance` falls back to immediate `set_def` for plugins without staged
  support, changing active config before `commit_group`;
- `commit_group` sequentially commits targets and has no rollback;
- a commit failure after earlier successes leaves a partially activated group;
- the system resumes dispatch after that partial commit;
- script params and plugin defs use separate mutation paths and cannot be
  activated as one transaction;
- config has no immutable revision identity;
- `get_def()` may mix persistent configuration and live telemetry;
- heavy assets in instance folders are mutable and not revision-bound;
- live tuning, saved project content, staged config, and active config are not
  clearly separated;
- there is no approval, actor, reason, or change-history model.

The current `commit_group` provides an important but narrower guarantee:

> No inspection runs while the selected commits execute.

It does not currently guarantee:

> Every selected target activates the new recipe, or none of them do.

The recommended model is:

> Store recipes as immutable bundles. Validate and prepare the complete bundle
> under a transaction ID. Activate only transaction-capable targets at one drain
> barrier. Make commit non-failing by contract after successful prepare. Bind the
> resulting active revision to every inspection.

Recipe catalog, approval workflow, branch graph, and UI should remain a
controller plugin concern. The core should own only the minimal activation
transaction and active revision because plugins cannot independently guarantee a
cross-instance run boundary.

## Scorecard

| Area | Score | Assessment |
|---|---:|---|
| Per-file persistence | 8/10 | Atomic write and working-copy recovery are strong |
| Live single-value update | 7/10 | Safe for common non-reentrant cases, but unversioned and unaudited |
| Static validation | 5/10 | Manifest checks exist, often warning-only and incomplete |
| Multi-instance prepare | 5/10 | Useful staged primitive, inconsistent fallback semantics |
| Multi-instance activation | 4/10 | Drain barrier exists, no all-or-none guarantee |
| Script parameter transaction | 2/10 | Parameters update individually with no staging bank |
| Heavy asset integrity | 3/10 | Folder path exists, assets are mutable and not content-bound |
| Active revision identity | 2/10 | No canonical config revision or activation epoch |
| Rollback | 2/10 | Working-copy filesystem recovery exists; runtime config rollback does not |
| Approval/change control | 1/10 | No actor, reason, approval, or lifecycle contract |
| Audit history | 2/10 | Logs/warnings exist, no append-only config change ledger |
| Inspection binding | 2/10 | Results do not identify active config revision |
| Overall configuration integrity | 4/10 | Good primitives, incomplete transaction semantics |

## Design Guardrails

1. **Keep recipe storage and UI out of the core.**
2. **Do not call a drain barrier a transaction by itself.**
3. **Do not silently degrade an atomic recipe request to immediate updates.**
4. **Do not use live telemetry returned by `get_def()` as canonical config.**
5. **Do not mutate shared asset folders in place for approved recipes.**
6. **Do not treat runtime working state as recipe configuration.**
7. **Do not make commit perform expensive or failure-prone work.**
8. **Do not resume production after partial activation without a fault policy.**
9. **Do not overwrite the prior active revision before the new one is proven.**
10. **Do not infer approval from a file existing on disk.**
11. **Do not make live tuning automatically equivalent to approved production
    config.**
12. **Do not promise rollback for arbitrary plugins with irreversible side
    effects.**

## Configuration Taxonomy

The current project uses the word “state” for several different things. A
coherent integrity model needs explicit categories.

| Category | Examples | Lifecycle |
|---|---|---|
| Pipeline structure | Script, plugin instances, dispatch groups | Project revision |
| Script parameters | Threshold, sigma, limits | Config revision |
| Instance definition | Detector settings, camera exposure | Config revision |
| Heavy assets | Model, template, calibration, mask | Asset revision |
| Runtime state | Counters, tracker history, caches | Ephemeral/migrated state |
| Telemetry | Frames processed, temperature, last score | Observation only |
| Operational controls | Start/stop, trigger mode | Runtime command |
| Governance metadata | Author, approver, reason, effective date | Recipe catalog |

Only script parameters, persistent instance definition, and referenced immutable
assets belong in a recipe bundle.

Runtime state and telemetry must not enter the recipe merely because a plugin
returns them from `get_def()`.

## Findings

### 1. Active, saved, staged, and edited configuration are not distinct

At any moment the system may have:

- canonical project files;
- a `.xinsp_work` editing copy;
- unsaved live parameter changes;
- saved instance JSON;
- an active in-memory plugin definition;
- a prepared heavy-resource slot;
- a script parameter replay cache;
- a last-good loaded script using older defaults;
- mutable files inside an instance resource folder.

These are different facts. The UI and protocol do not provide a single model that
answers:

- What is active?
- What is saved?
- What is edited but not saved?
- What is prepared but not active?
- What was approved?
- What will survive restart?

#### Recommendation

Expose explicit revision slots:

```text
canonical_revision
working_revision
active_config_revision
pending_config_revision
approved_recipe_revision
```

The system should never use one `dirty` or `saved` flag to represent all of them.

#### Acceptance Criteria

- An operator can identify active versus saved configuration.
- A prepared revision is visible without being described as active.
- Restart behavior names which revision will be restored.
- Inspection evidence carries only the active revision.

### 2. `load_project` is a sequential partial update

The `load_project` command:

1. reads a JSON file;
2. iterates script params and calls `set_param`;
3. iterates instance definitions and calls `set_def`;
4. records warnings for failures;
5. returns `ok: true` with warning arrays when part of the load failed.

Continuous dispatch is not quiesced around the entire operation. A run may
observe:

- some new params and old instance defs;
- all new params and only some new instance defs;
- a failed instance retaining its previous or partially mutated config;
- script-owned and backend-managed instances changing at different moments.

This is acceptable for an explicitly named best-effort import. It is not
acceptable as a recipe activation primitive.

#### Recommendation

Split the command semantics:

- `import_config_best_effort`: current behavior, clearly non-atomic;
- `prepare_recipe`: validate and stage all values/assets;
- `activate_recipe`: all-or-none activation contract;
- `save_snapshot`: persist a snapshot without implying activation.

Do not preserve ambiguous `load_project` as the production recipe verb.

#### Acceptance Criteria

- Atomic activation never uses sequential live mutation.
- Best-effort import is visibly labeled and blocked in production mode unless
  explicitly permitted.
- A failed prepare leaves active config unchanged.

### 3. Partial loads return protocol success

`load_project` returns `ok: true` even when `param_warnings` or
`instance_warnings` are non-empty.

This is better than silently hiding failures, but it still creates
fail-reads-as-success behavior in generic clients:

```text
if response.ok:
    show "Recipe loaded"
```

The difference between:

- complete success;
- success with ignored unknown fields;
- partial activation;
- no active change;
- failed validation;

must be machine-readable at the top level.

#### Recommendation

Use explicit result states:

```json
{
  "status": "prepared|activated|partial|rejected|unchanged",
  "transaction_id": "...",
  "target_revision": "...",
  "active_revision": "...",
  "failures": []
}
```

For production activation, `partial` is a failure and must not resume ordinary
inspection automatically.

### 4. `prepare_instance` has two incompatible meanings

For plugins implementing staged ABI:

```text
prepare = build pending state without changing active state
```

For plugins without staged ABI:

```text
prepare = immediate gated set_def
commit = no-op
```

This fallback is convenient for simple plugins but breaks transaction
composition. In a mixed group:

1. legacy/simple plugin A changes during prepare;
2. staged plugin B remains old;
3. inspection continues between prepare and commit;
4. runs observe A-new/B-old;
5. commit later switches B.

The orchestrator cannot claim a frame-perfect group switch.

#### Recommendation

Declare plugin activation capability:

```text
immediate
staged-atomic
staged-atomic-abortable
```

An atomic recipe transaction must reject targets that lack the required
capability. It must not silently downgrade.

For simple scalar plugins, the SDK can provide a default staged wrapper so the
author still writes little code:

```cpp
xi::StagedConfig<MyConfig>
```

#### Acceptance Criteria

- Capability is discoverable before prepare.
- Atomic transactions contain only compatible targets.
- Immediate fallback is used only for explicitly non-atomic live tuning.

### 5. `commit_group` prevents crossing runs but is not all-or-none

`commit_group` correctly:

- stops new dispatch;
- drains in-flight work;
- invokes each target in a no-process window;
- resumes the prior dispatch mode.

It then commits targets sequentially. If target A succeeds and B fails:

- A remains new;
- B remains old or faulted;
- later targets may continue committing;
- response reports failure;
- dispatch resumes when the guard exits.

The next run can therefore use a partial recipe.

#### Recommendation

Strengthen the commit contract:

1. `prepare` performs all parsing, allocation, I/O, and validation.
2. `ready(transaction_id)` proves staged state exists.
3. `commit(transaction_id)` is `noexcept` and limited to pointer/index swap.
4. All targets are ready before the barrier closes.
5. Barrier commits every target.
6. Active revision changes only after every commit returns.

If a supposedly non-failing commit faults, the runtime enters a latched
configuration fault and does not resume production automatically.

#### Acceptance Criteria

- Ordinary validation/load errors occur before the barrier.
- A successful commit cannot return a business failure.
- Unexpected commit fault leaves the line stopped/faulted.
- Active revision advances once, after the whole group commits.

### 6. There is no abort operation for staged configuration

Repeated prepare may overwrite a plugin's staging slot. A failed group prepare
can leave successfully prepared targets holding pending resources indefinitely.
There is no common:

```text
abort(transaction_id)
```

contract.

This creates resource leaks, ambiguity, and accidental later commits of stale
pending data.

#### Recommendation

Add an optional but standard abort operation:

```text
prepare(tx, config, asset_root)
validate(tx)
commit(tx)
abort(tx)
```

Rules:

- only one pending transaction per instance unless plugin declares otherwise;
- a newer transaction cannot silently replace an older pending transaction;
- abort is idempotent;
- project close/reload aborts pending transactions;
- staged resource memory and files have bounded lifetime.

### 7. Transactions have no identity

`prepare_instance` and `commit_group` are correlated only by target names and
current plugin staging slots.

Without a transaction ID:

- two controllers can interfere;
- a delayed commit may activate the wrong prepare;
- retry cannot be distinguished from a new operation;
- target readiness cannot be queried coherently;
- logs cannot reconstruct a config switch;
- stale staged data can be committed accidentally.

#### Recommendation

Every recipe operation receives a host-issued transaction ID:

```text
tx_id
base_revision
target_revision
target_manifest
actor/reason
created_at
expires_at
```

Prepare, validate, commit, abort, status, and audit records all use this ID.

#### Acceptance Criteria

- A commit cannot activate staging from another transaction.
- Retried requests are idempotent.
- Concurrent controllers receive a conflict rather than racing.
- Expired pending transactions are aborted safely.

### 8. There is no optimistic concurrency control

A controller can read current definitions, edit a bundle, and apply it after the
active config changed in the meantime.

Without `expected_active_revision`, the later write silently overwrites an
unseen change.

#### Recommendation

Require compare-and-swap semantics for recipe activation:

```json
{
  "expected_active_revision": "sha256:old",
  "target_revision": "sha256:new"
}
```

Reject on mismatch unless the caller explicitly requests a force operation with
appropriate policy and audit reason.

### 9. Script params cannot participate in group activation

`xi::Param<T>` values are atomics updated one at a time by `set_param`. They are
safe individual reads, but multiple params do not change as one logical set.

A script may read:

```text
new lower threshold
old upper threshold
```

if an inspection runs between updates.

Script params also use a separate replay cache from plugin instance definitions,
so a recipe transaction cannot currently bind both.

#### Recommendation

Introduce a script config snapshot:

```cpp
struct InspectionConfig {
    int lower;
    int upper;
    double sigma;
};

xi::ConfigSnapshot<InspectionConfig> config;
```

Prepare builds an immutable snapshot; commit swaps one pointer at the same group
barrier as plugin commits.

Keep individual `Param` for development live tuning. Production recipe activation
should use a snapshot bank or staged parameter registry.

#### Acceptance Criteria

- Related params can activate atomically.
- One run reads one config snapshot.
- Live tuning mode and governed recipe mode are distinguishable.

### 10. `get_def()` is not a clean canonical configuration API

The instance reference documentation states that:

- `config` need not match `get_def()` shape;
- `set_def` may ignore unknown keys;
- `get_def` may include live telemetry such as processed-frame counters.

That makes `get_instance_def -> set_instance_def` an unreliable basis for a
canonical recipe bundle:

- telemetry creates a new hash without a config change;
- read-only fields may be passed back into `set_def`;
- omitted defaults may canonicalize differently;
- plugin-defined key order or float formatting may change hashes;
- a plugin can accept only part of what it returns.

#### Recommendation

Separate interfaces:

```text
get_config()       persistent, canonical, settable fields only
validate_config()  pure validation/normalization
get_status()       telemetry and readiness
get_schema()       config schema/capabilities
```

`get_def()` can remain a compatibility wrapper, but recipe tooling should not
depend on mixed config/status output.

#### Acceptance Criteria

- `get_config -> normalize -> set_config -> get_config` is stable.
- Telemetry never changes config revision.
- Canonical serialization is deterministic.

### 11. Manifest validation is advisory and not authoritative

On project open, manifest params are checked for key/type/range issues. Warnings
do not block loading, and `set_def()` remains the plugin's actual authority.

This reflects a flexible plugin ecosystem, but production recipe activation
requires stricter semantics:

- unknown keys may be typos;
- bad values may be ignored or clamped;
- missing fields may default;
- plugin validation can mutate state before returning false;
- manifest and implementation may drift.

#### Recommendation

Use two validation levels:

- **Development:** warn and allow.
- **Production recipe:** reject unknown, missing-required, wrong-type, out-of-range,
  schema-version, and asset-reference errors before prepare.

Plugin `validate_config()` should be pure and return normalized config plus
structured field errors.

```json
{
  "valid": false,
  "errors": [
    {
      "path": "/instances/det0/threshold",
      "code": "out_of_range",
      "expected": "[0,255]",
      "actual": 300
    }
  ]
}
```

### 12. Plugin setters are not required to be transactional internally

`set_def()` returns a boolean, but the contract does not structurally prevent:

1. mutating field A;
2. failing to parse field B;
3. returning false;
4. leaving A changed.

The host may report failure while the plugin is partially modified.

#### Recommendation

Document and test a strict setter contract:

> Parse and validate into a temporary immutable config. Publish it only after
> complete success.

Provide SDK helpers that naturally enforce this pattern. Add generated contract
tests:

- rejected config leaves `get_config()` byte-equivalent;
- unknown field policy is honored;
- normalization is deterministic;
- repeated application is idempotent.

### 13. Heavy assets are mutable folders, not immutable revisions

Instance folders may contain:

- calibration files;
- weights;
- templates;
- captured images;
- plugin scratch data.

The same folder is both persistent asset storage and mutable plugin workspace.
A recipe can point to a folder, but there is no manifest proving which files were
loaded. A file can change in place after approval or between prepare and commit.

#### Recommendation

Separate:

```text
assets/<content-addressed immutable revision>/
workspace/<instance mutable scratch>/
```

Recipe manifests reference immutable asset revisions:

```json
{
  "asset": {
    "id": "sha256:...",
    "files": [
      { "path": "model.onnx", "sha256": "...", "size": 1829942 }
    ]
  }
}
```

Prepare verifies the manifest before loading. Approved assets are never modified
in place; creating a new calibration creates a new revision.

### 14. Runtime state and recipe configuration can contaminate each other

`xi::state()` persists across script hot reload and supports schema migration.
Plugin `get_def()` may also expose counters or live state. Recipe switching should
not accidentally restore:

- tracker history;
- counters;
- caches;
- last result;
- temporary calibration sessions;
- source worker position.

The roadmap correctly says runtime working state is not part of a config set, but
the interfaces do not fully enforce this distinction.

#### Recommendation

Recipe activation declares runtime-state policy:

```text
preserve
reset
migrate(old_recipe, new_recipe)
plugin-defined
```

Default to reset for state whose meaning depends on recipe. Preserve only when
the author declares compatibility.

State migration and recipe migration are separate operations:

- code schema migration reshapes state across code versions;
- recipe activation decides whether old operational state remains meaningful.

### 15. Persistence is file-safe but not revisioned

`atomic_write` protects individual JSON files. The working-copy journal can roll
forward an interrupted multi-file mirror. These are strong durability mechanisms.

They do not provide:

- immutable history;
- revision IDs;
- rollback snapshots;
- parent relationships;
- actor/reason metadata;
- semantic diff;
- approval lifecycle.

Roll-forward heals the requested save; it does not restore the previous recipe.

#### Recommendation

Build a recipe catalog in a controller plugin:

```text
recipe revision
├── immutable manifest
├── parent revision
├── normalized configs
├── immutable asset references
├── author/time/reason
├── validation report
├── approval records
└── activation history
```

The core consumes a prepared bundle and exposes active revision. It does not own
the catalog.

### 16. Rollback is undefined at runtime

Working-copy discard is an editor/filesystem operation. Runtime rollback after a
bad recipe activation needs different semantics.

Challenges:

- some plugins may have irreversible side effects;
- an immediate plugin may already have changed;
- a camera or PLC plugin may have sent external commands;
- runtime state may have reset;
- old assets may have been reclaimed;
- rollback itself can fail.

#### Recommendation

Define rollback classes:

| Class | Guarantee |
|---|---|
| R0 | No runtime rollback; stop and require operator recovery |
| R1 | Reactivate previously prepared immutable revision |
| R2 | Reactivate revision plus compatible runtime-state snapshot |
| R3 | Compensating external actions defined by integration plugin |

For staged pure-compute plugins, R1 should be normal: retain the previous active
slot until the new revision passes post-activation health checks.

Do not promise universal rollback across arbitrary external side effects.

### 17. Post-activation verification is absent

A commit can succeed mechanically while the new recipe is unusable:

- camera stops delivering;
- model loads but produces invalid outputs;
- calibration does not match sensor;
- thresholds reject every part;
- PLC mapping is incompatible;
- latency exceeds takt.

#### Recommendation

Recipe activation should support:

```text
prepare
validate
commit
verify
promote active
```

Possible verification:

- plugin health/readiness;
- expected source heartbeat;
- dry-run/golden sample;
- schema compatibility;
- latency budget;
- sanity bounds;
- controller-plugin domain checks.

Until verification passes, status is `active-unverified`, not fully active.

### 18. Live tuning and governed production changes are the same mutation paths

The same `set_param` and `set_instance_def` paths serve:

- developer experimentation;
- operator tuning;
- production recipe change;
- automated orchestration.

These contexts need different rules.

#### Recommendation

Define modes:

| Mode | Behavior |
|---|---|
| Development | Immediate changes allowed, dirty state visible |
| Commissioning | Changes tracked, snapshot/promote workflow |
| Production | Only approved recipe activation; direct mutation restricted |
| Service override | Time-limited authorized change with mandatory reason |

The backend can enforce mutation policy through capabilities/authorization. The
controller plugin owns approval workflow.

### 19. There is no actor, reason, or audit history

Current mutations identify command and target in logs, but not:

- who changed it;
- why;
- old value;
- new value;
- approval;
- effective inspection boundary;
- whether change was saved;
- whether it survived restart.

#### Recommendation

Append config events:

```json
{
  "type": "config.activation",
  "transaction_id": "...",
  "actor": "engineer-17",
  "reason": "Product change A -> B",
  "base_revision": "...",
  "target_revision": "...",
  "requested_at": "...",
  "activated_at": "...",
  "effective_from_inspection_id": "...",
  "result": "activated"
}
```

Direct development tuning can use a lower-assurance actor such as local session,
but should still leave a useful session history.

### 20. Active config is not bound to run outcomes

Even a perfect recipe transaction is insufficient if inspection results do not
record which config revision was active.

The traceability review proposed a runtime revision ID. Config activation should
advance its active config component at one exact inspection boundary.

#### Recommendation

The commit coordinator records:

```text
old active config revision
target config revision
last inspection under old revision
first inspection under new revision
```

Each accepted run snapshots the active revision ID cheaply. No per-run config
serialization is required.

## Proposed Recipe Bundle

```json
{
  "schema": "xi.recipe/1",
  "recipe_id": "product-b",
  "revision": "sha256:...",
  "parent_revision": "sha256:...",
  "pipeline_revision": "sha256:...",
  "configs": {
    "script": {
      "lower": 80,
      "upper": 160,
      "sigma": 2.5
    },
    "instances": {
      "camera0": {
        "schema": "camera.config/2",
        "value": {
          "exposure_us": 1200
        }
      },
      "detector0": {
        "schema": "blob.config/3",
        "value": {
          "threshold": 128,
          "min_area": 50
        },
        "asset_revision": "sha256:..."
      }
    }
  },
  "metadata": {
    "product": "B",
    "station_class": "vision-line-v2",
    "author": "engineer-17",
    "created_at": "2026-07-02T10:00:00Z",
    "reason": "Initial production release"
  },
  "approvals": [
    {
      "actor": "quality-03",
      "at": "2026-07-02T12:00:00Z",
      "scope": "production"
    }
  ]
}
```

Revision hashing uses a documented canonical JSON form and immutable asset
references. Approval metadata may or may not participate in content identity,
but the policy must be explicit.

## Proposed Activation Protocol

### Begin

```json
{
  "command": "begin_config_transaction",
  "expected_active_revision": "sha256:old",
  "target_revision": "sha256:new",
  "targets": ["script", "camera0", "detector0"]
}
```

Response:

```json
{
  "transaction_id": "tx-...",
  "status": "created",
  "active_revision": "sha256:old"
}
```

### Prepare

The coordinator:

1. resolves the immutable bundle;
2. validates schemas and target existence;
3. verifies asset hashes;
4. checks transaction capability;
5. prepares every target;
6. aborts all prepared targets on any failure;
7. records a structured validation report.

### Commit

The coordinator:

1. rechecks expected active revision;
2. verifies every target ready for the same transaction;
3. closes and drains dispatch;
4. invokes non-failing commit on every target;
5. swaps active config revision;
6. records the effective inspection boundary;
7. resumes dispatch;
8. starts post-activation verification.

### Abort

Abort:

- is idempotent;
- clears every staged target for the transaction;
- never changes active config;
- records actor/reason/failure;
- occurs automatically on expiration, project close, or failed prepare.

## Integrity Invariants

1. Active config always has an immutable revision ID.
2. Every accepted inspection binds one active config revision.
3. Prepare never changes active config in atomic mode.
4. All targets prepare successfully before dispatch closes.
5. Commit performs no parsing, I/O, allocation, or validation.
6. Successful commit advances active revision exactly once.
7. Unexpected commit fault leaves production stopped.
8. Abort is idempotent and leaves active revision unchanged.
9. A transaction commits only its own staged state.
10. Recipe assets are immutable and hash-verified.
11. Runtime telemetry never affects config identity.
12. Rejected validation cannot partially mutate active plugin state.
13. Direct live tuning is visibly different from approved recipe activation.
14. Actor, reason, base revision, target revision, and effective boundary are
    auditable.
15. Rollback guarantee is declared per target and transaction.

## Prioritized Roadmap

### P0: Correct Semantics and Naming

1. Document `load_project` as best-effort, non-atomic mutation.
2. Stop describing mixed-capability `prepare -> commit_group` as group-atomic.
3. Expose whether each target is immediate or staged.
4. Return explicit `partial` status instead of generic success with warnings.
5. Do not automatically resume production after partial commit.
6. Separate config from telemetry in documentation and templates.

### P1: Add Revision Identity

1. Define canonical config serialization.
2. Add active and pending config revision IDs.
3. Add transaction ID and expected base revision.
4. Bind active config revision to run outcomes.
5. Distinguish canonical, working, active, and approved revisions.
6. Add structured config-change events.

### P2: Make Prepare Strict

1. Add capability discovery.
2. Reject atomic transactions containing immediate-only targets.
3. Add pure validation and normalized config result.
4. Add `ready(tx)` and `abort(tx)`.
5. Add pending transaction expiry and conflict handling.
6. Add generated setter atomicity tests.

### P3: Make Commit Non-Failing

1. Restrict commit to a prepared pointer/index swap.
2. Make commit contract `noexcept`.
3. Verify all targets before closing dispatch.
4. Latch config fault on unexpected commit failure.
5. Advance active revision only after complete commit.
6. Record exact inspection boundary.

### P4: Include Script and Assets

1. Add staged script parameter snapshot.
2. Commit script params at the same barrier as plugin configs.
3. Separate immutable assets from mutable instance workspace.
4. Add asset manifests and hash verification.
5. Add runtime-state preserve/reset/migrate policy.

### P5: Governance and Rollback

1. Implement recipe catalog/controller plugin.
2. Add draft, validated, approved, active, retired lifecycle.
3. Add actor/reason/approval records.
4. Add R0-R3 rollback capability declarations.
5. Add post-activation verification and promotion.
6. Restrict direct mutation in production mode.

## Suggested First Implementation Slice

The first implementation should improve truth without building the full recipe
catalog:

1. Add `config_revision` counter/hash to the backend runtime.
2. Add `transaction_id` to prepare and commit commands.
3. Report per-instance activation capability.
4. Reject atomic group prepare when any target is immediate-only.
5. Add abort for staged targets.
6. Require all targets ready before `commit_group`.
7. On unexpected commit failure, keep dispatch stopped and publish a latched
   config fault.
8. Advance active revision only after full success.
9. Include config revision in `run_result`.
10. Add tests for mixed capability, failed prepare, stale transaction, concurrent
    controller, partial commit fault, abort, restart, and inspection boundary.

This slice establishes honest atomic semantics. Immutable recipe storage and
approval workflow can then be built in a controller plugin.

## Decision Checklist

### Configuration Meaning

- Is this value config, runtime state, telemetry, or an operational command?
- Is the serialized form canonical and stable?
- Can it be round-tripped without adding telemetry?

### Validation

- Is validation pure?
- Are unknown and missing fields handled explicitly?
- Does failure leave active state unchanged?
- Are assets present and hash-verified?

### Transaction

- What is the transaction ID?
- What active revision is expected?
- Can every target stage without changing active state?
- Can commit fail after prepare?
- Is abort idempotent?

### Activation

- Does dispatch remain closed for the complete commit?
- What happens on an unexpected target fault?
- When does active revision advance?
- Which inspection is the first under the new revision?

### Persistence

- Is the recipe immutable and revisioned?
- Is the active revision also saved for restart?
- Is working-copy save being confused with activation?
- Can the previous revision be reconstructed?

### Governance

- Who requested and approved the change?
- Why was it changed?
- Is direct live tuning allowed in this mode?
- Is a service override time-limited and auditable?

### Rollback

- What rollback class does each target support?
- Are external side effects reversible?
- Is the previous asset/config slot retained?
- Does failed verification automatically stop or revert?

## Success Metrics

- Percentage of production inspections with active config revision.
- Number of partial activation incidents.
- Number of atomic requests rejected due to incompatible targets.
- Config prepare and commit latency distributions.
- Pending transaction expiry/abort count.
- Validation failures caught before dispatch closure.
- Direct production mutation attempts blocked or audited.
- Time required to identify active versus saved recipe.
- Rollback success rate by declared class.
- Number of plugins passing setter non-mutation contract tests.

## Final Judgment

xInsp2 has already solved several difficult mechanical problems: safe per-instance
mutation, background resource preparation, frame-boundary draining, crash-safe
file writes, and working-copy recovery.

The remaining gap is semantic precision.

Today the system can safely change many individual configurations. It cannot yet
guarantee that a multi-instance recipe became active as one all-or-none revision.
The current fallback and failure behavior can produce partial activation while
the protocol reports only that some targets failed.

The correct boundary is:

- a controller plugin owns recipe catalog, version graph, approval, and UI;
- plugins own validation and immutable prepared resource slots;
- the core owns transaction identity, the cross-instance drain boundary, active
  revision, and failure policy;
- inspection outcomes bind the active revision.

First make atomic activation claims true. Then add catalogs, approvals, rollback,
and richer orchestration.
