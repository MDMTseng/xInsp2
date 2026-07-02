"""
run_qa.py — run the examples/qa_*/driver.py regression suite sequentially.

    python tools/run_qa.py [filter] [--timeout=N] [--list] [--strict]

Each driver prints a `VERDICT: PASS|FAIL: …` line (and may print `SKIP: …`). The
runner aggregates PASS/FAIL/SKIP, prints a summary, and exits non-zero if any
test FAILed (so it's usable as a CI gate). Tests run SEQUENTIALLY — each spawns
its own backend(s) and some share project-plugin DLL paths, so they must not
overlap. Windows-first; the drivers SKIP themselves on non-nt.

  python tools/run_qa.py              # all qa_* tests
  python tools/run_qa.py group        # only those whose folder name contains "group"
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
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXAMPLES = REPO / "examples"
QUARANTINE_FILE = EXAMPLES / "qa_known_failing.txt"


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


def discover(filt: str):
    return [d / "driver.py" for d in sorted(EXAMPLES.glob("qa_*"))
            if (d / "driver.py").exists() and (not filt or filt in d.name)]


def verdict_of(text: str) -> str:
    v = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("VERDICT:"):
            v = "PASS" if "PASS" in s else "FAIL"
        elif s.startswith("SKIP:") and v is None:
            v = "SKIP"
    return v or "FAIL"   # no verdict printed = treat as failure (driver crashed)


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: (a.split("=", 1)[1] if "=" in a else True)
            for a in sys.argv[1:] if a.startswith("--")}
    filt = args[0] if args else ""
    timeout = int(opts.get("--timeout", 360))
    strict = bool(opts.get("--strict"))
    quarantine = {} if strict else load_quarantine()
    tests = discover(filt)
    if opts.get("--list"):
        for t in tests: print(t.parent.name)
        return 0
    if not tests:
        print(f"no qa_* tests match {filt!r}"); return 1

    if quarantine:
        print(f"quarantine: {len(quarantine)} known-failing test(s) allowed to fail "
              f"(examples/qa_known_failing.txt); a quarantined PASS is fatal. "
              f"Use --strict to ignore.\n")
    print(f"running {len(tests)} qa test(s)" + (f" matching {filt!r}" if filt else "") + " ...\n")
    results = []
    for drv in tests:
        name = drv.parent.name
        sys.stdout.write(f"  {name:26s} ... "); sys.stdout.flush()
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
        # Reclassify against the quarantine: an expected failure becomes a loud
        # (non-fatal) KNOWN-FAIL; an unexpected pass of a quarantined test is a
        # fatal signal that its line must be removed (the list can't rot green).
        if v in ("FAIL", "TIMEOUT") and quar:
            outcome = "KNOWN-FAIL"
        elif v == "PASS" and quar:
            outcome = "UNEXPECTED-PASS"
        else:
            outcome = v
        tag = {"PASS": "PASS", "FAIL": "FAIL <<<", "SKIP": "skip",
               "TIMEOUT": "TIMEOUT <<<", "KNOWN-FAIL": "KNOWN-FAIL",
               "UNEXPECTED-PASS": "UNEXPECTED PASS <<<"}.get(outcome, outcome)
        print(f"{tag}   ({dt:.0f}s)")
        if outcome == "KNOWN-FAIL":
            print(f"      quarantined: {quarantine[name]}")
        elif outcome == "UNEXPECTED-PASS":
            print(f"      remove '{name}' from examples/qa_known_failing.txt "
                  f"(it passes now: {quarantine[name]})")
        if v in ("FAIL", "TIMEOUT") and not quar and r is not None:
            for line in (r.stdout or "").splitlines()[-6:]:
                print("      " + line)
        results.append((name, outcome, dt))
        time.sleep(1.0)   # let the prior test's backend release its port before the next

    npass = sum(1 for _, o, _ in results if o == "PASS")
    nfail = sum(1 for _, o, _ in results if o in ("FAIL", "TIMEOUT"))
    nskip = sum(1 for _, o, _ in results if o == "SKIP")
    nknown = sum(1 for _, o, _ in results if o == "KNOWN-FAIL")
    nunexp = sum(1 for _, o, _ in results if o == "UNEXPECTED-PASS")
    print(f"\n=== {npass} passed, {nfail} failed, {nskip} skipped, "
          f"{nknown} known-fail, {nunexp} unexpected-pass "
          f"({len(results)} total, {sum(d for _, _, d in results):.0f}s) ===")
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
