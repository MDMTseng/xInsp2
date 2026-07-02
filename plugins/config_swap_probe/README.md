# config_swap_probe

Reference plugin for the orchestrator config-swap design
([docs/roadmap/config-bundles-and-orchestration.md](../../docs/roadmap/config-bundles-and-orchestration.md)).
It demonstrates the **double-slot (prepare/commit)** pattern a heavy-resource
plugin uses so its config can be swapped frame-perfectly without stalling the
pipeline while assets load: `prepare()` builds the (simulated heavy) resource in
a background staging slot, and `commit()` swaps staging → live in one atomic
pointer store.

Its `process()` is a no-op (it only records what value the live slot held), so it
has **no process input/output contract**. Its script/UI-facing surface is its
**config** (`{value}`), its `get_status` **command**, and the **status** object
that command returns — and that status is the only way the double-slot behaviour
is observable. It follows the plugin data contract on those surfaces.

## Keys — one source of truth

Every key name is defined **once** in
[`config_swap_probe_keys.h`](./config_swap_probe_keys.h); the plugin's own
readers and the typed view ([`config_swap_probe_io.h`](./config_swap_probe_io.h))
compile from it.

| Surface | Key | Type | Notes |
|---------|-----|------|-------|
| config  | `value`        | int | the loaded resource value (set_def / prepare) |
| command | `command`      | string | `get_status` |
| status  | `active`       | int | value in the LIVE slot |
| status  | `staged`       | bool | is a resource staged? |
| status  | `staged_value` | int | value in the STAGING slot (`-1` if none) |
| status  | `last_seen`    | int | value the last `process()` observed |
| status  | `proc`         | int | `process()` call count |

Schema version: `xi::config_swap_probe::kSchemaVersion` (currently **1**). A
config or `prepare` def built against a different version is rejected (`set_def`
returns false, `prepare` returns non-zero) and the live slot is left untouched.
A def with no `_schema` stamp is tolerated (legacy persisted `instance.json`).

There are **no required process inputs** to fail loud on; the schema-skew
rejection above is this plugin's structured-error surface.

## Using it from a driver

```cpp
#include "config_swap_probe_io.h"

host.set_def(p, xi::config_swap_probe::Config().value(42));            // tier-1 immediate
host.prepare(p, xi::config_swap_probe::Config().value(99), folder);   // stage in background
host.commit_group(...);                                               // frame-perfect swap

auto s = xi::config_swap_probe::Status{
    host.exchange(p, xi::config_swap_probe::Command::get_status()) };
assert(s.active() == 99 && !s.has_staged());
```

## Tests

`tests/test_config_swap_probe.cpp` asserts: the `Config`/`Command`/`Status`
happy path including the full `prepare`→`commit` double-slot (staged value
visible while the live slot is unchanged, then swapped on commit); and a config
schema skew → `set_def` rejects it, leaving the live value unchanged. Run via
`ctest -C Release -R config_swap_probe_test` from `plugins/build`.
