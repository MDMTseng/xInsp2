# Contributing to xInsp2

Welcome. This is the practical "where do I start" doc — environment setup,
branch policy, commit style, and how to run the test sweep before you
push.

## Prerequisites

- **Windows 11** (Linux is a longer-term port; the WS server, SEH layer,
  and `cl.exe`-driven script compile are Windows-first).
- **Visual Studio 2022 Build Tools** (or full VS 2022) for `cl.exe`,
  `vcvars64.bat`, and the C++20 standard library.
- **CMake ≥ 3.16**.
- **Node.js ≥ 18** for the VS Code extension build, SDK CLI, and
  integration tests.
- **VS Code** if you're going to touch the extension (you can build the
  backend without it).

Optional accelerators (auto-detected at configure time):
- **OpenCV 4.x** — default ops backend below IPP. Auto-detected at
  common Windows install paths.
- **Intel IPP** — fastest image ops + JPEG. Set `XINSP2_HAS_IPP=ON` and
  `IPP_ROOT`.
- **libjpeg-turbo** — fastest JPEG encoder. Set
  `XINSP2_HAS_TURBOJPEG=ON` and `TURBOJPEG_ROOT`.

## First build

```bat
cd xInsp2\backend
cmake -S . -B build -A x64
cmake --build build --config Release

cd ..\vscode-extension
npm install
node esbuild.mjs
```

`backend/build/Release/xinsp-backend.exe` is the WebSocket service.
`vscode-extension/out/extension.js` is the bundled extension.

To open VS Code with the extension live (dev host):

```bat
code --extensionDevelopmentPath=<repo>\vscode-extension <repo>\examples
```

## Running tests

Full sweep:

```bat
cd backend\build && ctest -C Release --output-on-failure
cd ..\..\vscode-extension && node --test test\*.test.mjs
node test\runUserJourney.mjs
```

See [`docs/testing.md`](docs/testing.md) for the full test surface and
how to add new tests.

## Branch policy

- **`master`** — protected, must build clean and pass tests.
- **Feature work** on a topic branch off master (`feature/foo`,
  `fix/bar`, `spike/baz`).
- **Spikes** that need to evolve over many commits get their own branch
  (e.g. `shm-process-isolation`); merge when the team agrees the
  approach has earned its place.
- **Force-push** only on personal feature branches; never on `master`.

## Commit style

- One topic per commit. **Body explains "why", not "what"** — the diff
  shows what.
- Subject under ~70 chars: `<area>: <short imperative>`.
- Body wraps around 72 chars; bullet lists are fine.
- Before commits that touch behaviour, **run the relevant tests** and
  mention the green count in the body if non-trivial.
- Co-author tag is welcome:
  `Co-Authored-By: <name> <email>`.

## Pull requests

- Title = single sentence; body = 2–4 bullets covering the change +
  test plan.
- Link to the doc you updated (or note "no doc impact" — but if you
  changed an API, you probably need a doc bump).
- Don't merge until CI is green.

## Coding style

- C++20, `clang-format` defaults. Headers under `backend/include/xi/`
  are header-only by design (the build is mostly `#include`-driven).
- Prefer existing `xi::*` headers over reinventing (e.g.
  `xi_atomic_io.hpp` for crash-safe writes, `xi_seh.hpp` for SEH
  translation).
- New plugins follow the C ABI in `xi_abi.h` — never expose C++ types
  across the plugin boundary.
- Comments explain **why** something is non-obvious. Don't comment what
  the code already says clearly.
- Logs go to stderr in the format `[xinsp2] <message>` so they show up
  in the extension's Output channel.

## Doc culture

- Code change with user-visible impact → update the doc in the same
  commit (or the next one). The repo's doc plan
  ([`docs/`](docs/)) only stays useful if everyone keeps it current.
- New backend cmd → update `docs/protocol.md`.
- New plugin / script feature → update the relevant guide in
  `docs/guides/`.
- New test → update `docs/testing.md`.
- Status change (milestone done, spike merged) → update
  `docs/status.md`.

## Asking for help

- The architectural overview in [`docs/architecture.md`](docs/architecture.md)
  has the "where do I look for X?" table.
- Most public APIs have header comments richer than the dedicated docs.
  When in doubt, read the header.
