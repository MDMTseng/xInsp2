# config_validation_demo

Exercises FL r6 deferred P2-3: validation of `instance.json.config`
against `plugin.json.manifest.params` on `cmd:open_project`.

`instances/inst_typo/instance.json` is deliberately bad in four
distinct ways. Opening this project should:

- still succeed (warnings, not errors),
- still instantiate both instances,
- expose four warnings via `cmd:open_project_warnings` — one for
  each malformed config field on `inst_typo`. `inst_clean` produces
  zero warnings.

## Run

Start `xinsp-backend.exe`, then:

    python driver.py

Driver prints the captured warnings and asserts every expected
warning kind (`unknown_config_key`, `type_mismatch`, `out_of_range`,
`not_in_enum`) appeared on `inst_typo`.

## Why it exists

multi_source_surge round-6 friction log P2-3 (see
`../multi_source_surge/FRICTION.md`) describes a typo'd
`"shape": "burts"` that silently fell through to the plugin's
default and burned debugging time. The validation code that fixes
that lives in `xi::PluginManager::open_project`; this demo is the
end-to-end smoke test that shows it works.
