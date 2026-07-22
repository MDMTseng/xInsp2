# json_source — example project

A GUI-edited JSON document driving a pipeline.

```
code .          # or: open this folder in VS Code with the xInsp2 extension
```

Open the `src` instance's UI, edit the document, and the next run reads your
edit: the script pulls `part_id` / `width_mm` / `pass_limit` straight off the
pack plane and turns them into a verdict. Change `pass_limit` to 10 and the
verdict flips to NG.

**What it shows**

- a pull source: the script ticks `src`, and the emit arrives as the *next*
  trigger (`t.is_active()` tells the two runs apart — see `inspect.cpp`)
- the JSON-value → pack-entry mapping (`../README.md` has the table)
- `$fault` as a normal pack: a document json_source cannot encode arrives as
  data, not as an exception

**Files**: `project.json` (2 instances), `inspect.cpp` (the script),
`instances/src/instance.json` (the shipped document), `driver.py` (the
regression check — `python tools/run_qa.py example_json_source`).
