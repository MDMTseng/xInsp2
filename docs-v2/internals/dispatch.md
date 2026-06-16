# Dispatch — how a trigger becomes a run

> **Scope:** SHIPPED design-of-record for the whole "trigger → run" path:
> the TriggerBus (emit_trigger + correlation policies Any/AllRequired/
> LeaderFollowers), the emit/fetch dispatch model (stage under res_id, id-only
> dispatch bypassing correlation, fetch-by-id, gap-free seq back-pressure), and
> dispatch groups (per-group concurrency cap + CPU priority; the unified lane model).
> **Status:** SKELETON.
> <!-- source: docs/design/emitter-fetch-model.md + docs/design/dispatch-groups.md + trigger-bus parts of docs/architecture.md -->

<!-- TODO P2: collect the three scattered sources into one "how dispatch works"
  doc. Keep the lane-model-shipped note from dispatch-groups; note group-priority
  knobs are gated on parallelism.groups. -->
