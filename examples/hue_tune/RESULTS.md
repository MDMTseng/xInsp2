# hue_tune — Friction Loop round 1

## Outcome
- Red sweep:   8/8
- Blue sweep:  8/8
- Green sweep: 8/8
- Total:       24/24
- VERDICT: PASS

The exchange-driven live-tuning path works end-to-end. Three sweeps over
the same compiled plugin instance (`det` of type `hue_counter`), with
config mutated only via `c.exchange_instance("det", {...})` between
sweeps, all hit their targets exactly. No `compile_and_load` /
`recompile_project_plugin` between sweeps — confirmed in the driver and
in the manual reading of frames_processed in the exchange responses
(it counted 0 → 8 → 16 across the three sweeps, single instance).

## Friction log

### F-1: backend wasn't running and no docs hint that the SDK won't start it
- Severity: P2 (annoying but not blocking)
- Root cause: docs gap
- What I tried: ran the driver; got `ConnectionRefusedError [WinError 10061]` from `websocket.create_connection`.
- What worked: launched `backend/build/Release/xinsp-backend.exe` manually in the background, then re-ran. The skill index hints at this ("Backend is auto-started by the VS Code extension; if not running, `backend/build/Release/xinsp-backend.exe &` works") but a top-level "if you're not in VS Code, start the backend yourself" line in `tools/xinsp2_py/README.md` (or in the SDK's `Client.connect()` error message itself) would shave a minute. The current ConnectionRefusedError tells me nothing about *what* is supposed to be on :7823.
- Time lost: ~2 minutes

### F-2: stb_image RGB vs OpenCV's BGR isn't called out anywhere I could find
- Severity: P2
- Root cause: docs gap
- What I tried: had to grep `backend/src/xi_image_io.cpp` to confirm `stbi_load(...)` (RGB order) so I'd know to use `cv::COLOR_RGB2HSV` not `BGR2HSV`. If I'd guessed wrong, red discs (RGB 255,0,0) would have been read as BGR (0,0,255) and HSV-mapped near H=120, the whole pipeline would have looked correct but with permuted color labels — a particularly nasty silent failure for new plugin authors.
- What worked: reading the source. Confirmed in `xi_io.hpp`: comment says "decoder runs on the host side" but doesn't say the host emits RGB. A line in `docs/guides/adding-a-plugin.md` under "Image ops" along the lines of *"`xi::imread` returns RGB-ordered pixels (host uses stb_image); use `cv::COLOR_RGB2*` not `BGR2*` when handing the Mat to OpenCV"* would prevent this trap.
- Time lost: ~3 minutes

### F-3: my own test-data trap — gradient bg saturated through the S floor
- Severity: P2 (self-inflicted, but the symptom looks like a plugin/SDK bug at first glance)
- Root cause: something else (test design)
- What I tried: first `generate.py` had a gradient `(60,80,200)..(200,160,60)`. `cv::inRange` with `S>=80, V>=80` accepted ~20-30% of the **background** in the blue sweep (because `S=255*(B-min)/B` for those pixels comes out near 178 — well over 80). The blue stripe became a giant noisy "blob" that connected-components either merged with disc(s) or whose noise components overran them. Result: blue sweep returned counts of 2/3/4 mixed.
- What worked: tightened the gradient to near-neutral grey (channel spread ≤ 12) so no bg pixel can clear `S>=80`. Both blue *and* red sweeps then hit 8/8 cleanly. The plugin's saturation floor is a sensible default — it's the test data that was wrong.
- Time lost: ~5 minutes
- Lesson worth surfacing: a "test-data sanity check" helper in the plugin (return the per-frame mean S of pixels NOT in any accepted region — i.e. the background's mean saturation) would have made this immediately obvious. Not strictly a friction-with-xInsp2; more a friction-with-myself.

### F-4: `xi::Json::object()` chained-set-then-`dump()` API is slightly clunky for build-and-return
- Severity: P2
- Root cause: API design (minor)
- What I tried: get_def() returns `xi::Json::object().set(...).set(...).dump()`. Works, but it's noisy compared to a brace-enclosed init list. Not blocking, copied the pattern from `local_contrast_detector` so it was easy to follow.
- What worked: matching the existing style. No friction beyond a passing wish for `xi::Json::dump({{"k","v"},...})`-style sugar.
- Time lost: ~0 minutes (not worth fixing)

## What was smooth

- **Plugin scaffolding pattern is well-trodden.** Copying the `local_contrast_detector` / `region_counter` shape (class : public xi::Plugin, `XI_PLUGIN_IMPL`, `pool_image()`, `cv::*` directly on `as_cv_mat()`) was almost paint-by-numbers. The "no-xi-ops, just OpenCV" direction is clear in the docs and consistent across the existing examples — exactly the kind of thing that lets a new plugin land in 15 minutes.
- **`exchange_instance()` is right there in the SDK.** No fishing through `c.call("exchange_instance", ...)` — the convenience wrapper is named exactly what you'd guess, and its docstring tells you the cmd is just a JSON dict.
- **`open_project` reply already lists plugins + instances.** Printed them at the top and immediately confirmed `('det','hue_counter')` was registered. No separate `list_instances` round-trip needed for the basic sanity check.
- **First compile worked.** Clean compile of `inspect.cpp` and the project plugin on a fresh `open_project`; no missing-include surprises, OpenCV is auto-pulled by `xi.hpp`/`xi_plugin_support.hpp` as advertised. The build pipeline did the right thing without any cl.exe-flag fiddling on my side.
- **Live tuning genuinely works without recompile.** This was the case-under-test, and it passed: three different exchange commands, three different observed configs, single instance, single compiled DLL. `frames_processed` in the get_def() response went 0 → 8 → 16 across sweeps, proving the same instance stayed live the whole time.
- **Per-frame `frame_path` via `c.run(frame_path=...)` + `xi::current_frame_path()`** is a way nicer pattern than the old "PNG-source plugin reading by index" flow. Driver-side path construction, script-side just reads the path. Wish more guides led with this version first.
