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
    docs      check_doc_coverage.py + check_retired_terms.py   (no build)
    build     configure (fresh if the cache is stale) + build backend
              Release + build the shipped plugins Release
    ctest     the full C++ ctest suite (unit + integration + the perf gates
              + script_selfcheck; also re-runs doc_coverage/retired_terms)
    fixtures  pytest tools/xinsp2_py/tests — the protocol golden-fixture
              round-trip (no live backend)
    qa        tools/run_qa.py — the examples/qa_* regression sweep
    fuzz      tests/fuzz/run_smoke.py — the black-box fuzz smoke, build-breaking

The `docs` stage is intentionally also covered by `ctest` (as the
doc_coverage / retired_terms ctests). Running the standalone scripts first
gives a ~2s pre-build "docs drifted" signal, and keeping them in ctest means a
bare `ctest` run is still complete on its own. The overlap is cheap and
deliberate.

Node integration suites (`vscode-extension/test/*.test.mjs`) are NOT in this
gate: they need `npm install` and a heavier toolchain, and are run separately
(see docs/testing.md). This gate is the C++/Python enforced core.

USAGE
    python tools/gate.py                 # run everything, stop on first failure
    python tools/gate.py --list          # list stages, run nothing
    python tools/gate.py --only ctest,fixtures
    python tools/gate.py --skip fuzz     # e.g. skip the ~45s fuzz smoke
    python tools/gate.py --skip build    # reuse an existing backend/build
    python tools/gate.py --keep-going    # run all stages, report at the end
    python tools/gate.py --port 7824     # WS port for the fuzz smoke (dev-box:
                                         #   dodge the VS Code extension on 7823)

Windows-first (MSVC / cl.exe, like the rest of the toolchain). Exits non-zero
if any selected stage fails, so it is usable directly as a CI gate.
"""
from __future__ import annotations

import argparse
import os
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

STAGE_ORDER = ["docs", "build", "ctest", "fixtures", "qa", "fuzz"]


class StageFail(Exception):
    """A stage failed; message is the human summary."""


def _run(cmd: list[str], *, cwd: Path = REPO, env: dict | None = None) -> None:
    """Run a subprocess streaming its output; raise StageFail on nonzero exit."""
    printable = " ".join(str(c) for c in cmd)
    print(f"    $ {printable}", flush=True)
    rc = subprocess.call([str(c) for c in cmd], cwd=str(cwd), env=env)
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


def _ensure_fresh_cache(build_dir: Path) -> None:
    """Wipe the build dir if its cache was generated for a DIFFERENT path.

    Worktrees share build-dir *names* but not paths; a CMakeCache.txt copied
    from (or left by) another worktree hard-errors configure with
    'is different than the directory ... where CMakeCache.txt was created'.
    Detect that and start clean instead of making a human debug it.
    """
    home = _cache_home(build_dir)
    want = str(build_dir.resolve()).replace("\\", "/").rstrip("/")
    if home is not None and home.lower() != want.lower():
        print(f"    stale CMakeCache (built for {home}); wiping {build_dir}",
              flush=True)
        shutil.rmtree(build_dir, ignore_errors=True)


def _cmake_project(src: Path, build: Path, targets: list[str] | None) -> None:
    _ensure_fresh_cache(build)
    if not (build / "CMakeCache.txt").exists():
        _run(["cmake", "-S", src, "-B", build, "-A", "x64"])
    cmd = ["cmake", "--build", build, "--config", "Release"]
    if targets:
        cmd += ["--target", *targets]
    _run(cmd)


# --- stages -----------------------------------------------------------------

def stage_docs(_args) -> None:
    """The derive-from-source doc freeze guards. Pure stdlib Python, no build."""
    _run([sys.executable, REPO / "tools" / "check_doc_coverage.py"])
    _run([sys.executable, REPO / "tools" / "check_retired_terms.py"])


def stage_build(_args) -> None:
    """Backend (Release) + the shipped plugin DLLs the qa/fuzz stages load."""
    _cmake_project(BACKEND, BACKEND_BUILD, targets=None)
    _cmake_project(PLUGINS, PLUGINS_BUILD, targets=None)


def stage_ctest(_args) -> None:
    """Full C++ suite: unit + integration + perf gates + doc gates + selfcheck."""
    _run(["ctest", "--test-dir", BACKEND_BUILD, "-C", "Release",
          "--output-on-failure"])


def stage_fixtures(_args) -> None:
    """Protocol golden-fixture round-trip (no live backend)."""
    _run([sys.executable, "-m", "pytest", "-q", REPO / "tools" / "xinsp2_py" / "tests"])


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
    "ctest": stage_ctest,
    "fixtures": stage_fixtures,
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
