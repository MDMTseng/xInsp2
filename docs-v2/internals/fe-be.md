# FE / BE split — supervisor, safe-state, crash history

> **Scope:** SHIPPED design-of-record. The frontend supervisor (`xinsp-fe.exe`)
> over the in-process backend compute core: process lifecycle, the `SafeStateSink`
> PLC seam, BE-crash → PLC via `host->set_safe_state`, crash history, and the VS
> Code managed/attach modes. (Process isolation + SHM were removed 2026-05.)
> **Status:** SKELETON.
> <!-- source: docs/design/fe-be-split.md (+ fe-be-split-test-plan.md as a linked appendix) -->

<!-- TODO P2: port + tighten. -->
