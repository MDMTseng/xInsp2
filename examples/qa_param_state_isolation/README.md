# qa_param_state_isolation — cross-project param/state leak (Bug #8)

Deterministic regression for the project-switch leak of tuned `Param` values
(and persisted `xi::state`) into the next project's script.

## The bug

The backend keeps process-global replay shadows in `service_main.cpp`:

- `g_param_cache` — every accepted `cmd:set_param` value, replayed into the
  next compiled script so operator-tuned sliders survive a hot-recompile.
- `g_persistent_state_json` / `g_persistent_state_schema` — the script's
  `xi::state()` snapshot, restored after a recompile.

These were cleared in exactly ONE place: the `unload_script` handler. The far
more common `open_project` / `close_project` switch path was never given the
same reset — and neither unloads the inspection script DLL (script lifecycle is
independent of the project's plugin DLLs). So switching from recipe A to recipe
B would:

1. capture A's `xi::state()` (the old script is still live), then
2. replay A's cached param values over any same-named `Param` B declares
   (e.g. `thresh`), running B's inspections with A's tuned values — wrong
   pass/fail verdicts, no error surfaced.

## The fix

Reset all three shadows on the project boundary in both `open_project` and
`close_project`, mirroring `unload_script`.

## The test

`projA` and `projB` are two distinct projects whose `inspect.cpp` each declare
`xi::Param<int> thresh{"thresh", 50, {0,255}}`. The driver:

1. opens A, compiles it, `set_param thresh=200` → asserts effective value 200
2. opens B, compiles it → asserts effective `thresh == 50` (B's own default)

Effective values are read via `cmd:list_params` (reports each Param's live
value). PASS = step 2 reads 50. Pre-fix it reads 200 (A's value leaked in).

```
python driver.py
```

TODO(linux): backend compile (cl.exe) is Windows-only; the driver SKIPs on
non-nt.
