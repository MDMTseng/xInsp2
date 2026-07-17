#!/usr/bin/env python3
"""gate.py — the ONE authoritative pre-merge gate for xInsp2.

This is the single command a maintainer runs before merging, and the exact
same command CI runs on windows-latest (`.github/workflows/ci.yml` just
provisions deps and calls this). Keeping one entry point is the whole point:
there is no second list of "what to run" that can drift from CI.

WHY THIS EXISTS
---------------
Every gate in the tree used to be good and run by nobody. `ctest`, the doc
freeze guards, `run_qa.py`, and the fuzz smoke were each invoked by hand, so
"green" meant "green the last time someone remembered on their box." The fuzz
smoke was literally built "to be wired into CI as a build-breaking net"
(`tests/fuzz/README.md`) and was wired to nothing. This script is that wiring:
it runs the whole enforced surface in order and fails loud on the first stage
that fails.

STAGES (in order; stop on first failure unless --keep-going)
    docs      check_doc_coverage.py + check_retired_terms.py +
              check_doc_debt.py (DOC_DEBT.md ledger) + check_doc_links.py
              (@doc backlinks) + the static contract legs:
              contract/validate.py (schema/fixture check) and
              baseline_gate.py (additive-vs-breaking)            (no build)
    build     configure (fresh if the cache is stale) + build backend
              Release + build the shipped plugins Release
    sdk       compile the SDK surface nothing else exercises: the host_mock
              CLI (sdk/host_mock) + a scaffold-and-compile of every plugin
              template (easy/medium/expert incl. their UI panels)
    ctest     the full C++ ctest suite (unit + integration + the perf gates
              + script_selfcheck; also re-runs doc_coverage/retired_terms) from
              backend/build, THEN the plugins/build pack suite (~16 tests incl.
              the red-team regressions use_pack_door_test / record_replay_pack_
              test — built by `build` but never previously run). contract_live
              is excluded here — the `live` stage below runs it under THIS
              gate's python with skipping forbidden
    fixtures  pytest tools/xinsp2_py/tests — the protocol golden-fixture
              round-trip (no live backend)
    live      contract/live_conformance.py — the third contract leg (live WS
              bytes vs schemas + the live-allowlist ratchet); spawns its own
              backend; a missing dep is a FAILURE here, never a skip
    qa        tools/run_qa.py — the examples/qa_* regression sweep
    fuzz      tests/fuzz/run_smoke.py — the black-box fuzz smoke, build-breaking

The `docs` stage is intentionally also covered by `ctest` (as the
doc_coverage / retired_terms ctests). Running the standalone scripts first
gives a ~2s pre-build "docs drifted" signal, and keeping them in ctest means a
bare `ctest` run is still complete on its own. The overlap is cheap and
deliberate. (`contract_live` is the one exception: it is NOT cheap — it cold-
compiles a script via cl.exe — so the gate runs it exactly once, in `live`.)

Node integration suites (`vscode-extension/test/*.test.mjs`) are NOT in this
gate: they need `npm install` and a heavier toolchain, and are run separately
(see docs/testing.md). This gate is the C++/Python enforced core. The `sdk`
stage does need a bare `node` on PATH (sdk/scaffold.mjs is zero-install, no
npm) — a missing node FAILS the stage loudly; it never skips.

USAGE
    python tools/gate.py                 # run everything, stop on first failure
    python tools/gate.py --list          # list stages, run nothing
    python tools/gate.py --only ctest,fixtures
    python tools/gate.py --skip fuzz     # e.g. skip the ~45s fuzz smoke
    python tools/gate.py --skip build    # reuse an existing backend/build
    python tools/gate.py --keep-going    # run all stages, report at the end
    python tools/gate.py --port 7824     # WS port for the fuzz smoke (dev-box:
                                         #   dodge the VS Code extension on 7823)

Windows-first (MSVC / cl.exe, like the rest of the toolchain), but the harness
itself is now platform-neutral and runs on the Linux/aarch64 port: it invokes
every Python leg through `sys.executable`, resolves the backend exe and the
plugin shared libraries by the host's real suffix (no `.exe`/`.dll` hard-coding
downstream), and builds with the same Ninja Multi-Config generator under g++.
The one Linux prerequisite is the interpreter: launch the gate with a venv that
has the e2e deps (`pytest`, `jsonschema`, `websocket-client`, and an editable
`tools/xinsp2_py`) so the `fixtures`/`live`/`qa`/`fuzz` legs import cleanly —
the system `/usr/bin/python` lacks them. Exits non-zero if any selected stage
fails, so it is usable directly as a CI gate.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BACKEND = REPO / "backend"
BACKEND_BUILD = BACKEND / "build"
PLUGINS = REPO / "plugins"
PLUGINS_BUILD = PLUGINS / "build"

# On a developer box the VS Code extension auto-grabs the single-client WS slot
# on the default port 7823 the instant a backend opens it, which 503s the fuzz
# client (tests/fuzz/README.md). Default the fuzz smoke to a port the extension
# is not watching; CI (no extension) can pass --port 7823 or just accept this.
DEFAULT_FUZZ_PORT = "7824"

STAGE_ORDER = ["docs", "build", "sdk", "ctest", "fixtures", "live", "qa", "fuzz"]


class StageFail(Exception):
    """A stage failed; message is the human summary."""


def _run(cmd: list[str], *, cwd: Path = REPO, env: dict | None = None) -> None:
    """Run a subprocess streaming its output; raise StageFail on nonzero exit."""
    printable = " ".join(str(c) for c in cmd)
    print(f"    $ {printable}", flush=True)
    try:
        rc = subprocess.call([str(c) for c in cmd], cwd=str(cwd), env=env)
    except OSError as e:
        # A missing tool (node, cmake, ...) must be a stage FAILURE with a
        # readable message — never a traceback and never a silent skip.
        raise StageFail(f"`{printable}` could not start: {e}") from e
    if rc != 0:
        raise StageFail(f"`{printable}` exited {rc}")


def _cache_home(build_dir: Path) -> str | None:
    """The directory a CMakeCache.txt was generated for, or None if no cache."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_CACHEFILE_DIR:INTERNAL="):
            return line.split("=", 1)[1].strip().replace("\\", "/").rstrip("/")
    return None


def _cache_generator(build_dir: Path) -> str | None:
    """The generator a CMakeCache.txt was created with, or None."""
    cache = build_dir / "CMakeCache.txt"
    if not cache.exists():
        return None
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("CMAKE_GENERATOR:INTERNAL="):
            return line.split("=", 1)[1].strip()
    return None


def _ensure_fresh_cache(build_dir: Path) -> None:
    """Wipe the build dir if its cache was generated for a DIFFERENT path OR a
    DIFFERENT generator.

    Worktrees share build-dir *names* but not paths; a CMakeCache.txt copied
    from (or left by) another worktree hard-errors configure with
    'is different than the directory ... where CMakeCache.txt was created'.
    A generator switch (VS -> Ninja Multi-Config) likewise hard-errors
    ('does not match the generator used previously'). Detect either and start
    clean instead of making a human debug it.
    """
    home = _cache_home(build_dir)
    want = str(build_dir.resolve()).replace("\\", "/").rstrip("/")
    want_gen = os.environ.get("XINSP2_CMAKE_GENERATOR", CMAKE_GENERATOR)
    have_gen = _cache_generator(build_dir)
    if home is not None and home.lower() != want.lower():
        print(f"    stale CMakeCache (built for {home}); wiping {build_dir}",
              flush=True)
        shutil.rmtree(build_dir, ignore_errors=True)
    elif have_gen is not None and have_gen != want_gen:
        print(f"    generator changed ({have_gen} -> {want_gen}); wiping {build_dir}",
              flush=True)
        shutil.rmtree(build_dir, ignore_errors=True)


CMAKE_GENERATOR = "Ninja Multi-Config"  # was "Visual Studio" (-A x64); Ninja + a
# compiler cache (ccache, wired in backend/CMakeLists) collapse the cold ~500s
# rebuild a reset worktree pays. Ninja Multi-Config keeps the `--config Release`
# workflow. It needs cl.exe on PATH (vcvars) — already required for the runtime
# script compile, and CI provides it via ilammy/msvc-dev-cmd. Override for a fallback
# to the VS generator with env XINSP2_CMAKE_GENERATOR="Visual Studio 17 2022".


def _cmake_project(src: Path, build: Path, targets: list[str] | None,
                   defines: list[str] | None = None) -> None:
    _ensure_fresh_cache(build)
    gen = os.environ.get("XINSP2_CMAKE_GENERATOR", CMAKE_GENERATOR)
    if not (build / "CMakeCache.txt").exists():
        cfg = ["cmake", "-S", src, "-B", build, "-G", gen]
        if gen.startswith("Visual Studio"):
            cfg += ["-A", "x64"]   # VS generator takes the arch via -A; Ninja via env
        _run([*cfg, *(defines or [])])
    cmd = ["cmake", "--build", build, "--config", "Release"]
    if targets:
        cmd += ["--target", *targets]
    _run(cmd)


# --- stages -----------------------------------------------------------------

def stage_docs(_args) -> None:
    """The derive-from-source doc freeze guards + the contract gates. Python, no build.

    The contract gates run here with XINSP2_REQUIRE_SCHEMA_GATE=1: inside THIS
    gate a missing jsonschema is a failure, not a skip — the ctest variant's
    polite self-skip once let four unmapped fixtures ride a green gate
    (2026-07-02)."""
    _run([sys.executable, REPO / "tools" / "check_doc_coverage.py"])
    _run([sys.executable, REPO / "tools" / "check_retired_terms.py"])
    _run([sys.executable, REPO / "tools" / "check_doc_debt.py"])
    _run([sys.executable, REPO / "tools" / "check_doc_links.py"])
    env = os.environ.copy()
    env["XINSP2_REQUIRE_SCHEMA_GATE"] = "1"
    _run([sys.executable, REPO / "contract" / "validate.py"], env=env)
    _run([sys.executable, REPO / "contract" / "baseline_gate.py"], env=env)


def stage_build(_args) -> None:
    """Backend (Release) + the shipped plugin DLLs the qa/fuzz stages load."""
    _cmake_project(BACKEND, BACKEND_BUILD, targets=None)
    _cmake_project(PLUGINS, PLUGINS_BUILD, targets=None)


def stage_sdk(_args) -> None:
    """Compile the SDK surface no other stage exercises (it rotted silently
    before this stage existed — nothing built host_mock or a scaffolded
    template, so an ABI/header change could break every new-plugin flow while
    the gate stayed green).

    Two legs:
      1. sdk/host_mock — the standalone xi_run_plugin CLI, built exactly the
         way its own header documents (own CMakeLists, -DXINSP2_ROOT).
      2. Every plugin template (easy/medium/expert): render it with the REAL
         scaffolder (node sdk/scaffold.mjs — the same code path the VS Code
         command uses), sanity-check the rendered output (plugin.json parses,
         no unexpanded {{PLACEHOLDER}}, medium/expert emit their ui/index.html
         panel), then compile the scaffolded plugin against this tree.

    Scaffold output lives under build/gate_sdk/ (gitignored via `build/`).
    The render overwrites sources every run, so the compiler genuinely
    re-exercises the templates each gate run; the CMake caches stay warm, so
    the warm-run cost is a few small rebuilds, not reconfigures.
    """
    xinsp2_root_def = f"-DXINSP2_ROOT={REPO}"
    _cmake_project(REPO / "sdk" / "host_mock",
                   REPO / "sdk" / "host_mock" / "build",
                   targets=None, defines=[xinsp2_root_def])

    placeholder = re.compile(r"\{\{[A-Z_]+\}\}")
    for tid in ("easy", "medium", "expert"):
        out = REPO / "build" / "gate_sdk" / tid
        out.mkdir(parents=True, exist_ok=True)
        _run(["node", REPO / "sdk" / "scaffold.mjs", out,
              "--name", f"gate_{tid}", "--template", tid, "--force"])

        # The renderer leaves unknown {{KEYS}} in place "to surface bugs" —
        # surface them HERE, not in a user's first scaffold.
        rendered = [out / "plugin.json", out / "src" / "plugin.cpp",
                    out / "CMakeLists.txt"]
        if tid in ("medium", "expert"):
            ui = out / "ui" / "index.html"
            if not ui.exists() or not ui.read_text(encoding="utf-8").strip():
                raise StageFail(f"template '{tid}' did not render its UI panel "
                                f"(missing/empty {ui})")
            rendered.append(ui)
        for f in rendered:
            if not f.exists():
                raise StageFail(f"template '{tid}' did not render {f}")
            hit = placeholder.search(f.read_text(encoding="utf-8"))
            if hit:
                raise StageFail(f"template '{tid}': unexpanded placeholder "
                                f"{hit.group(0)} left in {f}")
        try:
            json.loads((out / "plugin.json").read_text(encoding="utf-8"))
        except ValueError as e:
            raise StageFail(f"template '{tid}': rendered plugin.json is not "
                            f"valid JSON: {e}") from e

        _cmake_project(out, out / "build", targets=None,
                       defines=[xinsp2_root_def])


def stage_ctest(_args) -> None:
    """Full C++ suite: unit + integration + perf gates + doc gates + selfcheck.

    Two ctest trees, both built by the `build` stage:
      1. backend/build — the core suite. contract_live is excluded (-E): the
         `live` stage runs the same script once under THIS gate's python with
         XINSP2_REQUIRE_SCHEMA_GATE=1, which is strictly stronger than the ctest
         variant (that one runs under the configure-time Python3_EXECUTABLE and
         may self-skip if that interpreter lacks jsonschema/websocket-client).
         The session cold-compiles a script via cl.exe, so unlike the doc gates
         this overlap is NOT cheap — run it once.
      2. plugins/build — the ~16-test plugin pack suite (cap_imgcodec_test,
         expose_pack_test, pack_order_gate_test, record_save_pack_test, ...).
         These are BUILT by the build stage but were never RUN by the gate, so
         the red-team pack regressions — use_pack_door_test (§8b/F1: the
         use-after-swap door) and record_replay_pack_test (§G/F5) — rode a green
         gate untested. Run them here so any plugins ctest failure fails the
         gate. The plugins suite has no quarantine/exclusions of its own (the
         qa_* flakies quarantined via examples/qa_known_failing.txt are the
         `qa` stage's, not ctests)."""
    _run(["ctest", "--test-dir", BACKEND_BUILD, "-C", "Release",
          "--output-on-failure", "-E", "^contract_live$"])
    _run(["ctest", "--test-dir", PLUGINS_BUILD, "-C", "Release",
          "--output-on-failure"])


def stage_fixtures(_args) -> None:
    """Protocol golden-fixture round-trip (no live backend).

    cwd matters: run from tools/xinsp2_py so THIS tree's `xinsp2` package
    shadows any pip-editable install pointing at another checkout — from the
    repo root, `import xinsp2` resolves to that install and this gate would
    quietly test someone else's working copy."""
    _run([sys.executable, "-m", "pytest", "-q", "tests"],
         cwd=REPO / "tools" / "xinsp2_py")


def stage_live(_args) -> None:
    """Live-wire conformance — the THIRD contract leg (schemas <-> live bytes).

    contract/live_conformance.py spawns the freshly built backend itself,
    drives a representative session, validates every captured message against
    contract/schemas, and ratchets unschema'd messages against
    contract/live-allowlist.json (an unknown new wire message FAILS; the
    allowlist only ever shrinks).

    Run under sys.executable — the SAME interpreter as the rest of this gate —
    with XINSP2_REQUIRE_SCHEMA_GATE=1, so 'jsonschema not installed' or
    'backend not built' is a hard FAILURE here, never the polite self-skip the
    ctest variant allows (the class of gap that let four unmapped fixtures
    ride a green gate on 2026-07-02)."""
    env = os.environ.copy()
    env["XINSP2_REQUIRE_SCHEMA_GATE"] = "1"
    _run([sys.executable, REPO / "contract" / "live_conformance.py"], env=env)


def stage_qa(_args) -> None:
    """examples/qa_* regression sweep (each spawns + tears down its own backend).

    Honors the quarantine in examples/qa_known_failing.txt: pre-existing broken
    examples (today: the dead-API scripts owned by adoption item 4) are run but
    non-fatal with a loud KNOWN-FAIL line, so this gate catches NEW breakage now
    instead of waiting for the whole suite to be clean. See
    docs/ci-gate-known-failures.md. run_qa exits non-zero if a NON-quarantined
    test fails or a quarantined test unexpectedly passes.
    """
    _run([sys.executable, REPO / "tools" / "run_qa.py"])


def stage_fuzz(args) -> None:
    """Black-box fuzz smoke — the build-breaking net its own README prescribes."""
    env = os.environ.copy()
    env.setdefault("XINSP_WS_PORT", args.port)
    _run([sys.executable, REPO / "tests" / "fuzz" / "run_smoke.py"], env=env)


STAGES = {
    "docs": stage_docs,
    "build": stage_build,
    "sdk": stage_sdk,
    "ctest": stage_ctest,
    "fixtures": stage_fixtures,
    "live": stage_live,
    "qa": stage_qa,
    "fuzz": stage_fuzz,
}


def _selected(args) -> list[str]:
    stages = STAGE_ORDER
    if args.only:
        want = {s.strip() for s in args.only.split(",") if s.strip()}
        bad = want - set(STAGE_ORDER)
        if bad:
            sys.exit(f"unknown stage(s): {', '.join(sorted(bad))}")
        stages = [s for s in STAGE_ORDER if s in want]
    if args.skip:
        drop = {s.strip() for s in args.skip.split(",") if s.strip()}
        stages = [s for s in stages if s not in drop]
    return stages


def main() -> int:
    ap = argparse.ArgumentParser(description="xInsp2 authoritative pre-merge gate")
    ap.add_argument("--only", help="comma-separated stages to run (default: all)")
    ap.add_argument("--skip", help="comma-separated stages to skip")
    ap.add_argument("--keep-going", action="store_true",
                    help="run all selected stages even after a failure")
    ap.add_argument("--list", action="store_true", help="list stages and exit")
    ap.add_argument("--port", default=DEFAULT_FUZZ_PORT,
                    help=f"WS port for the fuzz smoke (default {DEFAULT_FUZZ_PORT})")
    args = ap.parse_args()

    stages = _selected(args)
    if args.list:
        for s in STAGE_ORDER:
            mark = "  " if s in stages else "  (skipped) "
            print(f"{mark}{s}")
        return 0

    print(f"xInsp2 gate -- {len(stages)} stage(s): {', '.join(stages)}\n")
    results: list[tuple[str, str, float]] = []
    failed = False
    for name in stages:
        print(f"==> {name}", flush=True)
        t0 = time.time()
        try:
            STAGES[name](args)
            status = "PASS"
        except StageFail as e:
            status = "FAIL"
            failed = True
            print(f"    !! {name}: {e}", flush=True)
        dt = time.time() - t0
        print(f"<== {name}: {status} ({dt:.0f}s)\n", flush=True)
        results.append((name, status, dt))
        if status == "FAIL" and not args.keep_going:
            break

    total = sum(dt for _, _, dt in results)
    print("=" * 60)
    print("GATE SUMMARY")
    print("=" * 60)
    for name, status, dt in results:
        flag = "" if status == "PASS" else "   <<< FAILED"
        print(f"  {name:10s} {status:4s} {dt:6.0f}s{flag}")
    not_run = [s for s in stages if s not in {n for n, _, _ in results}]
    for name in not_run:
        print(f"  {name:10s} ---- (not run: earlier stage failed)")
    print(f"\n  total {total:.0f}s")

    if failed:
        bad = ", ".join(n for n, s, _ in results if s == "FAIL")
        print(f"\nGATE FAILED: {bad}")
        return 1
    print("\nGATE PASSED -- safe to merge.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
