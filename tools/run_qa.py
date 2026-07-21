"""
run_qa.py — run the examples/qa_*/driver.py regression suite.

    python tools/run_qa.py [filter] [--timeout=N] [--jobs=N] [--list] [--strict]

Each driver prints a `VERDICT: PASS|FAIL: …` line (and may print `SKIP: …`). The
runner aggregates PASS/FAIL/SKIP, prints a summary, and exits non-zero if any
test FAILed (so it's usable as a CI gate).

PARALLELISM (--jobs, default 4; env XINSP2_QA_JOBS; or 1 to force fully serial)
Each driver spawns its own backend(s) on a free_port() port, and J1's per-PID
script work_dir removed the last shared-build-dir collision — so the
parallel-safe tests run concurrently in a pool of `jobs` workers. The tests named
in examples/qa_serial.txt assert on TIMING / concurrency-count / load and would
flake under CPU oversubscription, so they run SERIALLY in a second phase with
nothing else running. --jobs=1 runs everything in one sequential pass (the old
behaviour) and ignores the serial split.

  python tools/run_qa.py              # all qa_* tests (parallel pool + serial tail)
  python tools/run_qa.py group        # only those whose folder name contains "group"
  python tools/run_qa.py --jobs=1     # fully sequential (no parallelism)
  python tools/run_qa.py --list       # list matching tests, don't run
  python tools/run_qa.py --strict     # ignore the quarantine — every fail is fatal

QUARANTINE (examples/qa_known_failing.txt)
A test named in that file still RUNS, but a FAIL is reported loudly as
`KNOWN-FAIL (<reason>)` and does NOT make the suite exit non-zero — this is how
the day-1 gate stays usable on a base with pre-existing breakage (see
docs/ci-gate-known-failures.md). The list cannot rot green: a quarantined test
that PASSES is an `UNEXPECTED PASS` and IS fatal (delete its line). `--strict`
bypasses the file so every failure is fatal (the raw truth).
"""
from __future__ import annotations
import os, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXAMPLES = REPO / "examples"
QUARANTINE_FILE = EXAMPLES / "qa_known_failing.txt"
SERIAL_FILE = EXAMPLES / "qa_serial.txt"


def load_quarantine() -> dict[str, str]:
    """name -> reason for tests allowed to fail without failing the suite."""
    out: dict[str, str] = {}
    if not QUARANTINE_FILE.exists():
        return out
    for line in QUARANTINE_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        name, _, reason = s.partition(":")
        out[name.strip()] = reason.strip()
    return out


def load_serial() -> set[str]:
    """Names that must run in the serial phase (timing/concurrency-sensitive)."""
    out: set[str] = set()
    if not SERIAL_FILE.exists():
        return out
    for line in SERIAL_FILE.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if s and not s.startswith("#"):
            out.add(s)
    return out


def test_name(drv: Path) -> str:
    """The suite-visible name of a driver — also the quarantine / serial key.

    examples/qa_foo/driver.py        -> "qa_foo"
    plugins/foo/example/driver.py    -> "example_foo"   (the folder is always
    named "example", so the PLUGIN supplies the identity)
    """
    if drv.parent.name == "example":
        return f"example_{drv.parents[1].name}"
    return drv.parent.name


def discover(filt: str):
    """The qa_* regression suite PLUS every official plugin's shipped example.

    An example nobody executes rots into a broken first impression — this repo
    has the receipts. Running plugins/<name>/example the same way as a qa_* test
    is what keeps `plugins/<name>/example` an actual demo instead of a claim.
    """
    dirs = sorted(EXAMPLES.glob("qa_*")) + sorted((REPO / "plugins").glob("*/example"))
    return [d / "driver.py" for d in dirs
            if (d / "driver.py").exists() and (not filt or filt in test_name(d / "driver.py"))]


def verdict_of(text: str) -> str:
    v = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("VERDICT:"):
            v = "PASS" if "PASS" in s else "FAIL"
        elif s.startswith("SKIP:") and v is None:
            v = "SKIP"
    return v or "FAIL"   # no verdict printed = treat as failure (driver crashed)


def run_one(drv: Path, timeout: int, quarantine: dict[str, str]):
    """Run one driver, classify against the quarantine. Returns
    (name, outcome, dt, report_str) where report_str is the fully-formatted line
    (+ any detail lines) ready to print atomically — so parallel workers don't
    interleave their output."""
    name = test_name(drv)
    t0 = time.time()
    r = None
    try:
        r = subprocess.run([sys.executable, str(drv)], cwd=str(REPO),
                           capture_output=True, text=True, timeout=timeout)
        v = verdict_of((r.stdout or "") + (r.stderr or ""))
    except subprocess.TimeoutExpired:
        v = "TIMEOUT"
    dt = time.time() - t0
    quar = name in quarantine
    # A `flaky` entry (reason starts with "flaky") oscillates pass/fail by nature,
    # so NEITHER direction is fatal — but both stay loud (review 07 #8 class).
    flaky = quar and quarantine[name].lower().startswith("flaky")
    if v in ("FAIL", "TIMEOUT") and quar:
        outcome = "KNOWN-FLAKY" if flaky else "KNOWN-FAIL"
    elif v == "PASS" and quar:
        outcome = "FLAKY-PASS" if flaky else "UNEXPECTED-PASS"
    else:
        outcome = v
    tag = {"PASS": "PASS", "FAIL": "FAIL <<<", "SKIP": "skip",
           "TIMEOUT": "TIMEOUT <<<", "KNOWN-FAIL": "KNOWN-FAIL",
           "KNOWN-FLAKY": "KNOWN-FLAKY", "FLAKY-PASS": "PASS (flaky)",
           "UNEXPECTED-PASS": "UNEXPECTED PASS <<<"}.get(outcome, outcome)
    lines = [f"  {name:26s} ... {tag}   ({dt:.0f}s)"]
    if outcome in ("KNOWN-FAIL", "KNOWN-FLAKY"):
        lines.append(f"      quarantined: {quarantine[name]}")
    elif outcome == "UNEXPECTED-PASS":
        lines.append(f"      remove '{name}' from examples/qa_known_failing.txt "
                     f"(it passes now: {quarantine[name]})")
    if v in ("FAIL", "TIMEOUT") and not quar and r is not None:
        for line in (r.stdout or "").splitlines()[-6:]:
            lines.append("      " + line)
    return name, outcome, dt, "\n".join(lines)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: (a.split("=", 1)[1] if "=" in a else True)
            for a in sys.argv[1:] if a.startswith("--")}
    filt = args[0] if args else ""
    timeout = int(opts.get("--timeout", 360))
    # Default scales with cores but is capped low: each qa backend is itself
    # multi-threaded (dispatch workers + OMP + a cl.exe compile), so even the
    # parallel-safe tests must not oversubscribe. 16-core dev → 4; 4-core CI runner
    # → 2; 2-core → 1 (serial). Override with --jobs or XINSP2_QA_JOBS.
    default_jobs = min(4, max(1, (os.cpu_count() or 4) // 2))
    jobs = int(opts.get("--jobs", os.environ.get("XINSP2_QA_JOBS", default_jobs)))
    jobs = max(1, jobs)
    strict = bool(opts.get("--strict"))
    quarantine = {} if strict else load_quarantine()
    serial_names = load_serial()
    tests = discover(filt)
    if opts.get("--list"):
        for t in tests: print(test_name(t))
        return 0
    if not tests:
        print(f"no qa_* tests match {filt!r}"); return 1

    # jobs=1 → one sequential pass (old behaviour, no serial split).
    # jobs>1 → parallel pool over the parallel-safe tests, then a serial tail.
    if jobs > 1:
        serial = [t for t in tests if test_name(t) in serial_names]
        par    = [t for t in tests if test_name(t) not in serial_names]
    else:
        serial, par = [], tests

    if quarantine:
        print(f"quarantine: {len(quarantine)} known-failing test(s) allowed to fail "
              f"(examples/qa_known_failing.txt); a quarantined PASS is fatal. "
              f"Use --strict to ignore.")
    print(f"running {len(tests)} qa test(s)" + (f" matching {filt!r}" if filt else "")
          + (f": {len(par)} parallel (jobs={jobs}) + {len(serial)} serial "
             f"(examples/qa_serial.txt)" if jobs > 1 else " sequentially") + " ...\n")

    results = []
    # Phase 1 — parallel-safe tests in a bounded pool. Print each as it finishes
    # (report_str is pre-formatted, so lines never interleave).
    if par and jobs > 1:
        with ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = {ex.submit(run_one, drv, timeout, quarantine): drv for drv in par}
            for fut in as_completed(futs):
                name, outcome, dt, report = fut.result()
                print(report)
                results.append((name, outcome, dt))
        if serial:
            print()
    # Phase 2 — serial (timing-sensitive) tests, one at a time, nothing else running.
    for drv in serial or (par if jobs == 1 else []):
        name, outcome, dt, report = run_one(drv, timeout, quarantine)
        print(report)
        results.append((name, outcome, dt))

    npass = sum(1 for _, o, _ in results if o == "PASS")
    nfail = sum(1 for _, o, _ in results if o in ("FAIL", "TIMEOUT"))
    nskip = sum(1 for _, o, _ in results if o == "SKIP")
    nknown = sum(1 for _, o, _ in results if o == "KNOWN-FAIL")
    nflaky = sum(1 for _, o, _ in results if o in ("KNOWN-FLAKY", "FLAKY-PASS"))
    nunexp = sum(1 for _, o, _ in results if o == "UNEXPECTED-PASS")
    # Wall time is now < sum-of-durations because of the parallel phase; report both.
    wall = sum(d for _, _, d in results)
    print(f"\n=== {npass} passed, {nfail} failed, {nskip} skipped, "
          f"{nknown} known-fail, {nflaky} flaky, {nunexp} unexpected-pass "
          f"({len(results)} total, {wall:.0f}s cpu) ===")
    if nfail:
        print("FAILED: " + ", ".join(n for n, o, _ in results if o in ("FAIL", "TIMEOUT")))
    if nknown:
        print("KNOWN-FAIL (quarantined, non-fatal — see examples/qa_known_failing.txt): "
              + ", ".join(n for n, o, _ in results if o == "KNOWN-FAIL"))
    if nunexp:
        print("UNEXPECTED PASS (delete these lines from examples/qa_known_failing.txt): "
              + ", ".join(n for n, o, _ in results if o == "UNEXPECTED-PASS"))
    # Fatal iff a NON-quarantined test failed, or a quarantined test unexpectedly
    # passed (its stale line must go). Quarantined failures alone are not fatal.
    return 0 if (nfail == 0 and nunexp == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
