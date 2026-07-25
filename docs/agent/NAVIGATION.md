# Backend navigation card (for coding agents)

A "I want to find X → do Y" lookup for the xInsp2 backend. Every recipe below was
verified by running the grep/lookup it names. **Check the generated map first:**
[`backend/src/MAP.md`](../../backend/src/MAP.md) already lists every TU's
responsibility, every WS command → handler → file, and every plugin's flags —
regenerate it with `python tools/gen_backend_map.py`. Design docs are numbered
under [`docs/new_gen/`](../new_gen) (e.g. `14` = lib/capability plane, `30` =
self-describing blob plane, `37` = pluginlet model).

## Commands & dispatch

- **Find the handler for a WS command** → open MAP.md table ②, or grep the
  dispatch table directly: `grep -n '"the_command"' backend/src/service_main.cpp`
  (the `g_cmd_table` literal maps `"cmd" → cmd_xxx_`). Then find the definition:
  `grep -rn 'void cmd_xxx_(xi::ws::Server' backend/src/`.
- **Is feature X a WS command or a plugin?** → if it appears in `g_cmd_table`
  (`grep -rn 'g_cmd_table' backend/src/`) it's a core command. If instead it's an
  `exchange` verb, it's plugin-owned: clients reach it via the `exchange_instance`
  command (`cmd_exchange_instance_` in `service_cmd_dispatch.cpp`, which pulls the
  inner `"cmd"` string from args and forwards to the named instance). Search
  plugin verbs with `grep -rn '"command"' toolbox/*/plugin.json`.

## Plugins & UI

- **What does a plugin declare?** → `toolbox/<name>/plugin.json`: `lib`/`autoload`
  flags, `sink`, `has_ui`, `manifest.params/exchange/capabilities`. MAP.md table
  ③ summarizes all of them.
- **Find a plugin's UI half** → `has_ui:true` plugins ship a `toolbox/<name>/ui/`
  dir (e.g. `toolbox/expose/ui/index.html`).
- **Find a pluginlet (parasitic UI/logic half, doc 37)** →
  `toolbox/pluginlets/<name>/pluginlet.json` names both halves (`halves.native`
  symbol + `halves.ui.entry`). MAP.md table ④ lists them.
- **Find a UI widget implementation** → the built-in widget vocabulary is
  `WIDGET_TAG` in `toolbox/pluginlets/controls/ui/mount-schema.mjs`; custom widgets
  are wired via `registerWidget(name, impl)` in the same file. Widget elements
  live in `toolbox/pluginlets/controls/ui/widgets/xi-*.svelte`.

## Engine subsystems (header families)

Headers live in `backend/include/xi/`; the prefix tells you the subsystem:

- `xi_pm_*` = **plugin manager** (discovery/load/instances/project) — the class
  shell is `xi_pm_manager_core.hpp`.
- `xi_pack*` = **data plane** (the uniform keyed-buffer pack + xi.pack@1 door):
  `xi_pack.hpp` container, `xi_pack_abi.hpp` host door, `xi_pack_contract.hpp`
  fault/provenance.
- `xi_cap*` = **capability/lib plane** (doc 14); `xi_image*` = image type/pool/blob;
  `xi_script*` = script compile/load/ABI; `xi_ws_server.hpp` = WS transport;
  `xi_health*` = health contract. When unsure, read the one-liner in MAP.md table ①.

## Validation, state, tests

- **Where a def/schema key is validated** → `backend/include/xi/xi_config_validate.hpp`
  validates an instance's `config` JSON against `manifest.params` (emits
  `OpenWarning` diagnostics). Script/kv state shape is `xi_kv.hpp` (doc 16).
- **Find test coverage for a subsystem** → `backend/tests/test_<subsystem>*.cpp`
  (e.g. `test_cap_*.cpp` for the capability plane, `test_pack*` / `test_controls`),
  and each is registered as a ctest in `backend/CMakeLists.txt` (`grep -n
  'add_test' backend/CMakeLists.txt`). Doc/contract gates carry the `docs` /
  `contract` ctest labels.
