# data_output — example project

A plugin's config surface, and how a plugin refuses honestly.

**`data_output` does not write files.** It models the config surface of a
results writer (`output_dir` / `format` / `auto_save`) and its `save` verb is
deliberately unimplemented. For real persistence use **`record_save`**.

The script reads the config through `exchange({"command":"get_status"})`, then
calls `save` and checks the *shape* of the refusal:

```json
{ "error": "not_implemented", "key": "save" }
```

**What it shows**

- a def surface is readable and writable from a script, not UI-only state
- `xi::contract::fault_json` — an unimplemented verb answers with a
  machine-checkable error instead of a success-looking def

**Files**: `project.json`, `inspect.cpp`, `driver.py`
(`python tools/run_qa.py example_data_output`).
