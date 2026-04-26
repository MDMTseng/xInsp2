# xInsp2 Test Audit Report

Three parallel audit agents reviewed all test files against FRAMEWORK.md and BugAudit.md.

---

## Coverage Summary

- **26 of 28 bugs have zero test coverage**
- **43% of WS commands (10 of 23) have no test**
- **Zero error path tests** across all integration tests
- **No tests for:** xi::state(), xi::use(), multi-file compile, project open/restore, hot-reload state persistence
- **No example scripts demonstrate xi::use()** — the recommended pattern

---

## Per-File Assessment

### C++ Unit Tests

| File | Verdict | Issues |
|------|---------|--------|
| `test_xi_core.cpp` | Mostly sound | Missing: Future double-get, negative Image dims, const char* VAR, state() thread safety. Parallel wall-time check `dt < 130` flaky on loaded CI. |
| `test_protocol.cpp` | Sound but shallow | Missing: JSON injection via unescaped keys. Fixture file fallback silently passes if file missing. |

### Node Integration Tests

| File | Cleanup | Flake Risk | Error Paths | Missing |
|------|---------|------------|-------------|---------|
| `ws_basic` | ✅ | Low | None | — |
| `ws_run_vars` | ✅ | Low | None | Compile failure |
| `ws_compile_reload` | ✅ | Low | None | Hot-reload, unload_script |
| `ws_preview` | ✅ | Medium (5s timeout) | None | preview_instance cmd |
| `ws_trigger` | ✅ | **High** (sleep+count) | None | Start while running, stop when stopped |
| `ws_defect` | ✅ | Medium | None | Wrong var kinds |
| `ws_crash` | ✅ | Medium (sleep for log drain) | Partial | Error log check inside `if` — silent swallow passes |
| `ws_plugins` | ✅ | Low | None | Invalid plugin name, duplicate instance |
| `ws_project` | ✅ | Low | None | open_project, instance def restoration |

### E2E Tests

| File | Verdict |
|------|---------|
| `index.cjs` | Mostly "no crash" — checks commands registered, never verifies output values |
| `full_pipeline.cjs` | Checks ok:true, never checks var values or image content |
| Screenshots | Saved for manual review only, never machine-verified |

### Example Scripts

| Script | Pattern | Issue |
|--------|---------|-------|
| `user_script_example.cpp` | Old (script-owned Param) | OK for now |
| `user_with_instance.cpp` | Old (xi::Instance in DLL) | Instance dies on reload |
| `defect_detection.cpp` | Old (function-local static source) | Source dies on reload |
| `plugin_handle_demo.cpp` | Old (PluginHandle) | Instance dies on reload |
| `record_demo.cpp` | Old (inline test image) | OK, demo only |
| **None** | **xi::use()** | **No example of the recommended pattern** |

---

## Untested WS Commands

| Command | Test Exists? |
|---------|-------------|
| `ping` | ✅ ws_basic |
| `version` | ✅ ws_basic |
| `shutdown` | ✅ ws_basic (but doesn't verify worker join) |
| `compile_and_load` | ✅ ws_compile_reload |
| `unload_script` | ❌ |
| `run` | ✅ ws_run_vars |
| `start` / `stop` | ✅ ws_trigger |
| `set_param` | ✅ ws_compile_reload |
| `list_params` | ❌ |
| `list_instances` | ❌ |
| `set_instance_def` | ❌ (ws_project uses it but test fails) |
| `exchange_instance` | ✅ ws_plugins |
| `save_instance_config` | ✅ ws_plugins |
| `preview_instance` | ❌ |
| `process_instance` | ❌ |
| `list_plugins` | ✅ ws_plugins |
| `load_plugin` | ✅ ws_plugins |
| `create_project` | ✅ ws_plugins |
| `open_project` | ❌ |
| `create_instance` | ✅ ws_plugins |
| `get_project` | ❌ |
| `get_plugin_ui` | ❌ |
| `save_project` / `load_project` | ✅ ws_project (params only) |

---

## Bug ↔ Test Cross-Reference

| Bug | Has Test? |
|-----|-----------|
| A4-1 Command injection | ❌ |
| A2-6 Handle truncation | ❌ |
| A3-1 Worker dangling ptrs | ❌ |
| A3-7 Shutdown use-after-free | ❌ (shutdown tested but not worker join) |
| A1-1 Future double-get | ❌ |
| A1-6 Negative image dims | ❌ |
| A1-8 state() thread safety | ❌ |
| A3-2 cmd:run vs worker race | ❌ |
| A3-3 use_exchange no SEH | ❌ |
| A2-2 Handle leak on crash | ❌ |
| A2-5 Plugin ctor ABI | ❌ |
| A3-5 Output handle leak | ❌ |
| All MEDIUM/LOW bugs | ❌ |

---

## Structural Issues

1. **Client class copy-pasted 6 times** with slight variations (different timeout defaults, different nextNonLog filters). Should be a shared `test/helpers/client.mjs` module.

2. **No shared test fixtures** — each test spawns its own backend, connects, does work, kills. No way to run a shared backend for faster test suites.

3. **Timing-dependent assertions** — `sleep(N)` then count/check is fragile. Should use event-driven waits (poll until condition, with timeout).

4. **No negative/adversarial tests** — no malformed JSON, no oversized payloads, no rapid-fire commands, no concurrent connections.

---

## Top 10 Missing Tests (Priority Order)

| # | Test | Would Catch |
|---|------|-------------|
| 1 | **xi::state() survives hot-reload** | A1-8 state bugs, serialization |
| 2 | **xi::use() round-trip** | xi::use() callback plumbing, process flow |
| 3 | **Compile failure returns error** | Silent failure in all current tests |
| 4 | **Inspection output value verification** | Any script logic bug |
| 5 | **Multi-file compilation** | CompileRequest.extra_sources path |
| 6 | **Project open → auto-load instances** | Plugin auto-load, instance restore |
| 7 | **Hot-reload during continuous mode** | A3-1 dangling DLL pointers |
| 8 | **cmd:run rejected during continuous** | A3-2 race condition |
| 9 | **SEH crash + recovery + correct output** | SEH catch completeness |
| 10 | **Shutdown during continuous mode** | A3-7 use-after-free |

---

## Recommendations

1. **Extract shared test client** — one `Client` class with configurable timeouts
2. **Write xi::use() example script** — update FRAMEWORK.md to match
3. **Add compile failure test** — trivially send a bad .cpp path
4. **Add value assertion test** — after run, check specific var values, not just "ok"
5. **Add continuous + reload test** — start, compile, verify no crash
6. **Replace sleep+count with poll loops** — `while (!condition) await sleep(100)`
