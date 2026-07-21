# controls_demo — example project

A plugin whose entire config UI is declared in C++.

Open the `ctl` instance's UI. The tabs, the 12-column grid, the sliders, numpad,
toggle, enum, radio, readouts and the Reset button are not HTML and not
hand-written JSON — they are builder calls in `../controls_demo.cpp`, rendered
by the webui from the plugin's own def surface
(docs/new_gen/37-pluginlet-model.md).

The script ticks the plugin each run, so the **Ticks** readout climbs while you
watch. Drag **Frame rate** and **FPS in use** follows on the next run — that is
the script reading the operator's value back through the same surface.

**What it shows**

- the `controls` pluginlet: one declaration, rendered UI + typed read-back
- `exchange()` as the control channel, returning the def surface as its reply

**Files**: `project.json`, `inspect.cpp`, `driver.py`
(`python tools/run_qa.py example_controls_demo`).
