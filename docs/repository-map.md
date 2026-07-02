# Repository map

> **Status: shipped** — a factual map of what lives in this repo, who owns it,
> and where its release boundaries fall. Facts here are read from the actual
> package manifests (`package.json`, `pyproject.toml`, `backend/CMakeLists.txt`)
> as of this commit; anything not derivable from the repo is marked
> "unspecified".

The repo root has valid but non-obvious ownership and release boundaries: a
single tree holds a C++ service, a VS Code extension, a Svelte UI kit, a browser
HMI, C++ plugins, an SDK, a Python client, protocol fixtures, examples, tools and
tests. This page groups those directories into **four products**, then gives a
per-package fact table.

The one hard compatibility contract across everything is the **plugin ABI**
(`xi_host_api`), currently **frozen at v11** (`xi_plugin_abi_version()` /
`XI_ABI_VERSION`; see [`reference/c-abi.md`](./reference/c-abi.md) and
[`internals/adr-001-host-api-freeze.md`](./internals/adr-001-host-api-freeze.md)).
Package SemVer numbers do **not** gate plugin loading — the ABI version does.

## Versioning model

Packages **version independently** — each moves on its own SemVer track; there is
no single monolithic product version (declared in the root `README.md`). Current
versions read from the manifests:

| Package | Version | Source of truth |
|---|---|---|
| `xinsp-backend` (+ `xinsp-runner`, `xinsp-fe`) | 0.2.0 | `backend/CMakeLists.txt` (`XINSP2_VERSION`) |
| VS Code extension | 0.2.0 | `vscode-extension/package.json` |
| `ui-components` (`xi-components`) | 0.1.0 | `ui-components/package.json` |
| Python client (`xinsp2`) | 0.1.0 | `tools/xinsp2_py/pyproject.toml` |

> Note: an earlier plan expected the Python client at 0.2.0; the repo currently
> declares **0.1.0** in `pyproject.toml`. The `xinsp2/__init__.py` module carries
> no `__version__` constant (unspecified there — the manifest is authoritative).

All packages are **pre-1.0**: minor bumps may carry breaking changes, and there
are no external consumers yet (first-party only).

## The four products

- **Runtime** — the compute core and its faces: the C++ backend service, the
  headless runner, the FE supervisor, and the wire protocol/fixtures.
- **Authoring** — how a developer writes an inspection: the VS Code extension and
  the project/plugin SDK.
- **Integration** — the surfaces others build against: shared UI web components
  and the Python WS client library.
- **Operation** — running a line: the browser HMI and deployment tooling.

## Per-package table

### Runtime

| Package | Purpose / owner | Ships? | Version | Build | Test | Outputs | Compatibility boundary |
|---|---|---|---|---|---|---|---|
| `backend/` | The core: `xi::*` headers, WS server, sharded ImagePool, TriggerBus, SEH crash translation, JIT script/plugin compile. Owns `xinsp-backend.exe` (interactive WS server) + `xinsp-runner.exe` (headless production runner). | Yes | 0.2.0 | `cmake -S backend -B backend/build -A x64 …; cmake --build backend/build --config Release` | `ctest --test-dir backend/build -C Release` | `xinsp-backend.exe`, `xinsp-runner.exe` (+ auto-copied OpenCV/turbojpeg/IPP DLLs) | **Publishes plugin ABI v11**; requires OpenCV 4.x at build+runtime |
| `fe/` (built via `backend/`) | FE-supervisor header surface (`xi_fe_status`, crash history/report, respawn policy, safe-state). Spawns + monitors the backend, drives the line safe on backend death, respawns rate-limited. Header-only here; the `xinsp-fe.exe` target lives in `backend/CMakeLists.txt`. | Yes | 0.2.0 (shares `XINSP2_VERSION`) | Built as target `xinsp_fe` in the backend CMake build | Covered by backend `ctest` (FE status/crash-history unit tests) | `xinsp-fe.exe` (links neither `xi_core` nor OpenCV) | Reads BE heartbeat/status files; no ABI surface of its own |
| `protocol/` | WS protocol **fixtures** (`fixtures/cmd_run.json`, `vars_mixed.json`) used to pin the wire format. Reference doc is [`reference/ws-protocol.md`](./reference/ws-protocol.md). | Ships as test/reference data (not a binary) | unspecified (no manifest) | n/a | Consumed by protocol tests | JSON fixtures | WS wire format (see ws-protocol.md) |

### Authoring

| Package | Purpose / owner | Ships? | Version | Build | Test | Outputs | Compatibility boundary |
|---|---|---|---|---|---|---|---|
| `vscode-extension/` | VS Code integration: Instances/Params TreeView, CodeLens, viewer webviews, plugin UI webviews, run commands (multicam / record / headless). Auto-spawns the backend. | Yes | 0.2.0 | `cd vscode-extension && npm install && npm run build` (esbuild) | `npm run test:protocol`, `npm run test:integration` (+ E2E drivers under `test/`, run via node) | `.vsix` (via `@vscode/vsce`) | Talks WS to a 0.2.0 backend; pinned to the known-compatible row |
| `sdk/` | Plugin/project SDK: `scaffold.mjs`, `create_plugin.sh`, `cmake/` module, `templates/`, `host_mock/`, `testing/` helpers, worked `examples/`. Lets you author a plugin without touching the host. | Yes (dev tooling shipped in the tree/zip) | unspecified (no own manifest) | Per-plugin: `cmake -S <plugin> -B <plugin>/build -A x64; cmake --build … --config Release` | `xi_test.hpp` helpers in `testing/`; scaffold self-tests | Plugin DLLs (built against the SDK cmake module) | Plugins compile against **ABI v11** |

### Integration

| Package | Purpose / owner | Ships? | Version | Build | Test | Outputs | Compatibility boundary |
|---|---|---|---|---|---|---|---|
| `ui-components/` (`xi-components`) | Svelte-authored UI components that compile to standard **custom elements** (`<xi-*>`), plus a shared WS-client shim and the importable HMI dashboard. The build step is contained to this folder; consumers stay framework-free. | Yes | 0.1.0 | `npm run build` (vite; also copies `dist/xi-components.esm.js` → `hmi/lib/`) | `npm test` (node `--test` DOM suites), `npm run e2e` (Playwright) | `dist/xi-components.esm.js` (ESM custom-element bundle) | Consumed by HMI, plugin UIs, VS Code webviews via `<xi-*>` tags |
| `tools/xinsp2_py/` (`xinsp2`) | Python client wrapping the backend WS protocol — drives compile/run/inspect cycles, reads VARs + image previews, dumps run snapshots (for AI workflows). | Yes | 0.1.0 | `pip install .` (setuptools; requires Python ≥ 3.10, `websocket-client`) | unspecified (no test config in `pyproject.toml`; fixtures under `tests/`) | Installable `xinsp2` package (`Client`, `RunResult`, `RunSnapshot`, …) | Speaks the WS protocol (see ws-protocol.md) |

### Operation

| Package | Purpose / owner | Ships? | Version | Build | Test | Outputs | Compatibility boundary |
|---|---|---|---|---|---|---|---|
| `hmi/` | Standalone browser SPA operator dashboard (v1.0 — RUN mode). The single WS client of an FE-supervised backend; subscribes to live `vars` + image preview and renders a `dashboard.json`-described card grid. No build step in v1 — plain ES modules; imports `xi-components` from `lib/`. | Yes | 1.0 (per its README title; no package manifest) | No build step (plain `.mjs`); serve via `serve.mjs` / `serve.py` | unspecified (served + exercised manually / via ui-components suites) | Static SPA (`index.html` + `app.mjs`) | WS client of a 0.2.0 backend; consumes `xi-components` ESM |
| `tools/` | Deployment + release tooling: `build_release.mjs` (release zip), `export_bundle.py` / AOT bundle, plus the Python client above. | Yes (dev/release tooling) | unspecified | `node tools/build_release.mjs`, `python tools/export_bundle.py …` | unspecified | `release/xinsp2-<version>-win-x64.zip`; AOT project bundles | n/a |

### Shared / non-product

| Package | Purpose | Ships? |
|---|---|---|
| `docs/` | Architecture, overview, testing, protocol/ABI reference, guides, internals, roadmap, ext_review. | Docs (in-tree) |
| `examples/` | User-script examples + `crash_tests` + qa drivers (defect_detection, buffer_replay_demo, stereo_sync, …). | In-tree samples |
| `tests/` | Cross-cutting fuzz harnesses (`fuzz/harness_*.py`, `run_smoke.py`). | Test-only |
| `plugins/` | Shipped first-party plugins: `blob_analysis`, `cache`, `config_swap_probe`, `data_output`, `expose`, `json_source`, `mock_camera`, `record_save`, `synced_stereo`. Built via `plugins/CMakeLists.txt`. | Yes — plugin DLLs (**ABI v11**) |

## Known-compatible set

What we build and test together today (from the root `README.md`):

| Backend | Extension | ui-components | Python client | ABI |
|---|---|---|---|---|
| 0.2.0 | 0.2.0 | 0.1.0 | 0.1.0 | v11 |

Pin to this row until 1.0 and a formal support policy.

## Facts that could not be determined from the repo

- `sdk/`, `hmi/`, `protocol/`, and `tools/` carry **no own version manifest** — their
  versions are "unspecified" (HMI's README title says v1.0; the others inherit
  context, not a declared number).
- The **Python client** declares **0.1.0** (not 0.2.0 as some plans anticipated);
  `xinsp2/__init__.py` has no `__version__`. No test runner is declared in its
  `pyproject.toml`.
- The **HMI** has no automated test entry point of its own.
