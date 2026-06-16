# C ABI — plugin exports + host services

> **Scope:** the full stable C ABI in one place — the exports a plugin DLL must
> provide (`XI_PLUGIN_IMPL`) AND the `xi_host_api` service table the host hands
> back. ABI version history + the yyjson-layout / `json_fallback` load gate.
> **Status:** SKELETON.
> <!-- source: docs/reference/plugin-abi.md + docs/reference/host_api.md (merged) -->

<!-- TODO P2: merge the two halves of the contract readers bounce between today.
  - Plugin exports (create/destroy/process/exchange/get_def/set_def + abi/yyjson stamps).
  - xi_host_api table (image pool, log, instance_folder, emit_trigger, emit/fetch
    resource, set_safe_state, doc_chunk_* + doc_retain/release/refcount).
  - ABI version history (v1..v4) in ONE place + the load gate (version refuse +
    yyjson-layout refuse-unless-json_fallback).
  - xi_record / xi_record_out at the boundary (data/len + doc/out_doc).
-->
