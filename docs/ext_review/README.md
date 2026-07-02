# Codex Reviews

This directory contains advisory reviews produced by Codex.

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

## Review Backlog

| Priority | Review | Status | Intended scope |
|---:|---|---|---|
| 1 | Production Truth and Traceability | Completed | Inspection evidence, identity, provenance, reproducibility, audit continuity, and decision trust |
| 2 | Recipe and Configuration Integrity | Completed | Atomic activation, approval, revision binding, rollback, and parameter change history |
| 3 | Real-Time Performance and Determinism | Completed | Performance budgets, latency tails, benchmark validity, overload, and long-run stability |
| 4 | Upgrade, Compatibility, and Rollback | Planned | Runtime compatibility matrix, protocol negotiation, project migration, and release rollback |
| 5 | Data, Model, and Calibration Lifecycle | Planned | Asset identity, versioning, validation, deployment, and retention |
| 6 | Release Engineering and Supply Chain | Planned | Reproducible artifacts, signing, SBOM, dependency provenance, and release evidence |
| 7 | Test Evidence Quality | Planned | Release gates, fixture validity, failure injection, visual regression, and soak evidence |
| 8 | Commissioning and Field-Service UX | Planned | Machine setup, preflight, diagnostics, replacement, offline service, and remote support |
