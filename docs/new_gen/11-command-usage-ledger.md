# Command-usage ledger — 55 WS commands (in-tree evidence only)

**Purpose.** A decision aid for the cutover train, not a deletion list. Every one
of the 55 commands is registered in `backend/src/service_main.cpp` (`g_cmd_table`)
and documented in `docs/reference/ws-protocol.md` — so **all 55 are contract/wire
surface and none are deleted by this pass.** What this ledger answers is narrower:
*which commands have zero in-tree callers*, so the app team can cross-check that
set against **their own** scripts before anyone retires a command.

**Method — honest about its limits.**
- Callers were found by grepping quoted command literals (`"cmd"` / `'cmd'`) — the
  shape every in-tree consumer uses (`sendCmd('x')`, `self.call("x")`,
  `this.cmd("x")`). A command invoked by runtime string-building would be missed;
  none in-tree do that.
- Consumer set searched (exactly the brief's set):
  `vscode-extension/src`, `ui-components/src` + `hmi`, `tools/xinsp2_py`,
  `examples/**` (drivers) + `tools/run_qa.py` + `tools/gate.py` + `tests/fuzz`,
  `contract/live_conformance.py`.
- `docs/reference/ws-protocol.md` documents **all 55** (verified) — it is the
  contract surface, so it is not counted as a "caller".
- **In-tree only.** A zero-caller command is *not* dead: app-team scripts and
  ad-hoc operator tooling live outside this repo and are the presumed callers of
  the operator/diagnostic commands. Zero here means "confirm against your code,"
  not "remove."
- Noise excluded from the caller lists below: `*.pyc` byte-compiled copies and
  captured `xinsp-backend-*.json` run logs (data, not callers).

## Zero in-tree callers — cutover-train candidates (5)

These have **no** in-tree invocation anywhere in the consumer set. All five are
plausible external-operator surface; the app team must confirm before retiring.

| Command | Nature | Notes (why it's plausibly external) |
|---|---|---|
| `watchdog_status` | diagnostic | Watchdog/health polling. `examples/multi_source_surge2/FRICTION.md:125` notes a project "would have to query `cmd:watchdog_status` separately" — i.e. an anticipated operator call, never wired in-tree. |
| `unload_script` | lifecycle | Only referenced in prose (`qa_param_state_isolation` driver/README discuss its param-clearing semantics vs `close_project`); never invoked in-tree. |
| `unquarantine_plugin` | plugin-mgmt | Recovery action for a quarantined plugin — operator/HMI gesture; no in-tree caller. |
| `load_plugin` | plugin-mgmt | Explicit single-plugin load; in-tree flows use `rescan_plugins`/`rebuild_plugins` instead. No in-tree caller. |
| `get_project` | project read | Whole-project snapshot read; in-tree UIs use `get_state` / `list_instances` / `open_project` instead. No in-tree caller. |

## Full ledger (caller files per consumer group)

Columns: **vsc** = vscode-extension/src, **ui** = ui-components/src + hmi,
**py** = tools/xinsp2_py, **qa** = examples drivers + run_qa/gate + tests/fuzz,
**ctr** = contract/live_conformance.py. `-` = none. (docs = all 55.)

| # | Command | vsc | ui | py | qa | ctr |
|---|---|:--:|:--:|:--:|:--:|:--:|
| 1 | ping | - | ✓ | ✓ | ✓ | ✓ |
| 2 | version | - | ✓ | ✓ | ✓ | - |
| 3 | crash_reports | ✓ | - | - | - | - |
| 4 | clear_crash_reports | ✓ | - | - | - | - |
| 5 | set_watchdog_ms | ✓ | - | - | ✓ | - |
| 6 | set_process_priority | ✓ | - | - | ✓ | - |
| 7 | set_timer_fps | ✓ | - | - | ✓ | - |
| 8 | **watchdog_status** | - | - | - | - | - |
| 9 | graph_capture | ✓ | - | - | - | - |
| 10 | graph_snapshot | ✓ | - | - | - | - |
| 11 | shutdown | - | - | ✓ | ✓ | - |
| 12 | compile_and_load | ✓ | - | ✓ | ✓ | ✓ |
| 13 | **unload_script** | - | - | - | - | - |
| 14 | run | ✓ | ✓ | ✓ | ✓ | ✓ |
| 15 | start | ✓ | - | - | ✓ | ✓ |
| 16 | stop | ✓ | - | - | ✓ | ✓ |
| 17 | list_params | - | - | - | ✓ | - |
| 18 | set_param | - | - | ✓ | ✓ | - |
| 19 | list_instances | ✓ | ✓ | ✓ | - | ✓ |
| 20 | set_instance_def | - | ✓ | ✓ | - | ✓ |
| 21 | get_instance_def | - | ✓ | - | ✓ | - |
| 22 | exchange_instance | ✓ | ✓ | ✓ | ✓ | - |
| 23 | get_state | - | ✓ | - | - | - |
| 24 | prepare_instance | - | ✓ | - | - | - |
| 25 | commit_group | - | ✓ | ✓ | - | ✓ |
| 26 | save_project | ✓ | - | - | - | - |
| 27 | commit_working_copy | - | - | - | ✓ | - |
| 28 | discard_working_copy | - | - | - | ✓ | - |
| 29 | load_project | ✓ | - | ✓ | - | ✓ |
| 30 | list_plugins | ✓ | - | ✓ | - | - |
| 31 | recent_errors | - | - | ✓ | - | - |
| 32 | status | ✓ | ✓ | ✓ | ✓ | - |
| 33 | image_pool_stats | - | - | ✓ | - | - |
| 34 | rescan_plugins | ✓ | - | - | - | - |
| 35 | **unquarantine_plugin** | - | - | - | - | - |
| 36 | **load_plugin** | - | - | - | - | - |
| 37 | create_project | ✓ | - | - | - | - |
| 38 | open_project | ✓ | - | ✓ | ✓ | ✓ |
| 39 | close_project | ✓ | - | - | ✓ | - |
| 40 | export_project_plugin | ✓ | - | - | - | - |
| 41 | recompile_project_plugin | ✓ | - | ✓ | - | - |
| 42 | rebuild_plugins | ✓ | - | - | - | - |
| 43 | dispatch_stats | - | ✓ | - | ✓ | ✓ |
| 44 | metrics | - | - | ✓ | - | ✓ |
| 45 | open_project_warnings | - | - | - | ✓ | - |
| 46 | create_instance | ✓ | - | - | ✓ | - |
| 47 | remove_instance | ✓ | - | - | - | - |
| 48 | rename_instance | ✓ | - | - | - | - |
| 49 | **get_project** | - | - | - | - | - |
| 50 | save_instance_config | - | - | - | ✓ | - |
| 51 | get_plugin_ui | ✓ | - | - | - | - |
| 52 | get_dashboard | - | ✓ | - | ✓ | - |
| 53 | toolchain_health | ✓ | - | - | ✓ | - |
| 54 | set_toolchain_override | ✓ | - | - | - | - |
| 55 | get_health | ✓ | ✓ | - | - | ✓ |

## Single-consumer commands (informational — NOT retirement candidates)

Commands reachable from exactly one consumer group are healthy (that group owns
the feature); listed only so the app team knows the blast radius if that consumer
changes. Examples: the crash/graph/toolchain-override commands are vscode-only
(operator UI); `recent_errors` / `image_pool_stats` are py-client-only
(diagnostics); `get_state` / `prepare_instance` are ui-components-only (the
instance-editing panel). None are deletion candidates — they have a live caller.

## Part 2 — in-process dead-code sweep (result: NO deletions)

Separate from the command ledger above, the service TUs + `fe_main` + `runner_main`
were swept for in-process dead code (orphans from the `service_main` split and the
dispatch-table refactor, duplicated utilities, dead flags/fields, dead branches).
**Result: nothing dead — the domain is already tight.** Evidence:

- **Header helpers** (45 free functions declared in `service_internal.hpp`): every
  one has ≥1 live call site (min reference count 3 = decl + def + use). No orphan.
- **File-local `static` helpers** (dispatch/inspect/sinks/result/health/toolchain,
  `fe_main`, `runner_main`): every one has a real call site — several sit at exactly
  2 references (def + one caller), all confirmed live, not decl-only. No orphan from
  the split.
- **Duplicated utilities**: none to consolidate. All param/JSON plumbing already
  routes through the shared `xp::` protocol header (`get_string_field`,
  `json_escape[_into]`, `parse_cmd`, `dispatch_command_guarded`) — no per-TU copies.
- **`fe_main` `FeConfig`** (23 fields) and **`runner_main` `Args`** (5 fields): every
  field is read. `probe_interval_ms` / `probe_fail_max` are read-only internal tuning
  constants (no CLI/config setter) but live — not dead.
- **`Engine` struct fields** (counters + strings): every field is both written and
  read; `boot_id` / `station_id` / `dropped_lifetime` / `high_watermark_lifetime` /
  `watchdog_trips` / `script_generation` are emitted to clients (contract — keep).
- **No dead markers**: no `#if 0`, `[[deprecated]]`, or "unused" TODOs. The `(void)`
  casts in `service_dispatch.cpp` are Linux-stub bodies + intentional-discard, not dead.
- **One borderline, kept**: `xp::Cmd` (outbound-command envelope in `xi_protocol.hpp`)
  has no in-tree production caller — the service is server-side and receives via
  `ParsedCmd`. It is exercised by `test_protocol.cpp` (symmetric to the tested `Rsp`)
  and is the canonical C++ definition of the inbound-command wire shape, so it is
  **tested contract surface — not deleted.**

No source changed by this pass; the `refactor(service)!` deletion commit the brief
anticipated has an empty changeset (nothing to delete). Baseline (ctest / run_qa /
contract gates) is therefore untouched.
