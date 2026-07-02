# External Reviews

This directory contains advisory reviews produced by external reviewers
(01–05 by Codex; 06–09 by Claude). Each review names its reviewer in the
document metadata.

## Naming Convention

```text
NN-topic-review.md
```

- `NN` defines reading order.
- File names use lowercase kebab-case.
- Dates belong in document metadata, not file names.
- One review uses one H1 title.

## Document Format

Reviews should use this structure where applicable:

1. Metadata
2. Scope
3. Executive Summary
4. Scorecard
5. Findings
6. Prioritized Roadmap
7. Decision Checklist or Success Metrics
8. Final Judgment

Heading hierarchy:

- H1: document title
- H2: major section
- H3: finding, phase, or subsection
- H4: recommendation, consequence, or acceptance criteria

## Reviews

| Order | Document | Scope |
|---:|---|---|
| 1 | [`01-project-taste-review.md`](./01-project-taste-review.md) | Product coherence, UI, HMI, documentation, operations, security, and repository structure |
| 2 | [`02-core-and-developer-ux-review.md`](./02-core-and-developer-ux-review.md) | Compute core, plugin developer UX, and `inspect.cpp` developer UX |
| 3 | [`03-production-traceability-review.md`](./03-production-traceability-review.md) | Inspection identity, provenance, evidence continuity, reproducibility, audit persistence, and decision trust |
| 4 | [`04-recipe-configuration-integrity-review.md`](./04-recipe-configuration-integrity-review.md) | Configuration identity, validation, atomic activation, persistence, approval, rollback, and audit history |
| 5 | [`05-real-time-performance-determinism-review.md`](./05-real-time-performance-determinism-review.md) | Performance contracts, benchmark validity, latency tails, overload, scheduling, observation cost, and reproducibility |
| 6 | [`06-upgrade-compatibility-rollback-review.md`](./06-upgrade-compatibility-rollback-review.md) | Plugin ABI freeze and negotiation, protocol versioning, project file format identity and migration, extension/backend skew, release rollback |
| 7 | [`07-test-evidence-quality-review.md`](./07-test-evidence-quality-review.md) | Gate enforcement, fixture validity, failure injection, crash-isolation coverage, fuzz coverage, plugin-SDK testing story, QA gate |
| 8 | [`08-concurrency-memory-safety-review.md`](./08-concurrency-memory-safety-review.md) | Refcounted pools, trigger/owner thread-local state, hot-reload lifetimes, crash-isolation mechanics, dispatch/shutdown races, data races |
| 9 | [`09-protocol-command-surface-robustness-review.md`](./09-protocol-command-surface-robustness-review.md) | WebSocket transport hardening, command-handler input validation, error contracts, session lifecycle, path containment, resource exhaustion |
| 10 | [`10-client-architecture-protocol-consumption-review.md`](./10-client-architecture-protocol-consumption-review.md) | Per-client code quality (ui-components, hmi, vscode-extension, xinsp2_py), cross-client protocol duplication and drift, event-contract coverage, binary frame consistency |
| 11 | [`11-plugin-example-exemplar-quality-review.md`](./11-plugin-example-exemplar-quality-review.md) | Shipped plugins and examples as teaching exemplars: API currency, ABI-boundary discipline, JSON-parsing hygiene, template consistency |

## Review Backlog

| Priority | Review | Status | Intended scope |
|---:|---|---|---|
| 1 | Production Truth and Traceability | Completed | Inspection evidence, identity, provenance, reproducibility, audit continuity, and decision trust |
| 2 | Recipe and Configuration Integrity | Completed | Atomic activation, approval, revision binding, rollback, and parameter change history |
| 3 | Real-Time Performance and Determinism | Completed | Performance budgets, latency tails, benchmark validity, overload, and long-run stability |
| 4 | Upgrade, Compatibility, and Rollback | Completed | Runtime compatibility matrix, protocol negotiation, project migration, and release rollback |
| 5 | Data, Model, and Calibration Lifecycle | Planned | Asset identity, versioning, validation, deployment, and retention |
| 6 | Release Engineering and Supply Chain | Planned | Reproducible artifacts, signing, SBOM, dependency provenance, and release evidence |
| 7 | Test Evidence Quality | Completed | Release gates, fixture validity, failure injection, visual regression, and soak evidence |
| 8 | Commissioning and Field-Service UX | Planned | Machine setup, preflight, diagnostics, replacement, offline service, and remote support |
| 9 | Concurrency and Memory Safety | Completed | Refcounted pools, thread-local trigger/owner state, hot-reload lifetimes, crash-isolation mechanics, and shutdown races |
| 10 | Protocol and Command-Surface Robustness | Completed | Transport hardening, handler input validation, error contracts, path containment, and resource exhaustion |
| 11 | Client Architecture and Protocol Consumption | Completed | Per-client quality, cross-client protocol duplication, event-contract coverage, and binary frame consistency |
| 12 | Plugin and Example Exemplar Quality | Completed | API currency of examples, ABI-boundary discipline, parsing hygiene, and scaffold template consistency |
