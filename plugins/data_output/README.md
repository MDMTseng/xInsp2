# data_output

An **example** plugin that models the *config/command surface* of a results
writer — nothing more. It exposes an `output_dir` / `format` / `auto_save`
config def via `get_def`/`set_def` and answers a small `exchange` command set.

## Stub status

`save` is **not implemented**. It does not write any file. Instead of returning
a success-looking def (which would silently lie to a caller/UI), the `save`
command returns the framework's structured error shape:

```json
{"error":"not_implemented","key":"save"}
```

built via `xi::contract::fault_json` — the same reason-code vocabulary used for
contract failures elsewhere, so a UI/driver sees one error shape across channels.

If you need results actually persisted to disk, use the **`record_save`** plugin,
which is the real writer. `data_output` exists only to demonstrate the config and
command surface a writer-style plugin exposes.

This plugin has **no data plane by design** — it overrides no `process()` path
(neither the Record path nor the `xi.pack@1` Pack door), so the bilingual Pack
door is **N/A** for it: there is nothing to walk and nothing to mirror. For the
generic-sink reference — a plugin that walks any producer's Pack with zero
producer knowledge and mirrors its Record path field-for-field — see the
**`expose`** plugin (`plugins/expose/src/expose.cpp`).

## Commands (`exchange`)

| command          | behaviour                                             |
|------------------|-------------------------------------------------------|
| `get_status`     | returns the current config def                        |
| `set_output_dir` | applies `output_dir`/`format`/`auto_save` from the payload, returns the def |
| `save`           | returns `not_implemented` (see above)                 |
