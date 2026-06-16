# Data layer — yyjson + in-process doc pass-by-pointer + γ-4 refcount

> **Scope:** SHIPPED design-of-record. yyjson-only Record backing (cJSON removed),
> the in-process script↔plugin doc pass-by-pointer (zero serialize), the host
> doc-chunk pool, and γ-4 cross-ABI doc refcount (`DocRegistry`, share_out /
> adopt_shared, zero-copy cache both sides, the `json_fallback` load gate).
> **Status:** SKELETON.
> <!-- source: docs/design/data-layer.md -->

<!-- TODO P2: port. Per PLAN open question #4: move the retained-MessagePack
  appendix to an archive note rather than keeping it inline. -->
