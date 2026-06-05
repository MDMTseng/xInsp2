"""
run_qa.py — run the examples/qa_*/driver.py regression suite sequentially.

    python tools/run_qa.py [filter] [--timeout=N] [--list]

Each driver prints a `VERDICT: PASS|FAIL: …` line (and may print `SKIP: …`). The
runner aggregates PASS/FAIL/SKIP, prints a summary, and exits non-zero if any
test FAILed (so it's usable as a CI gate). Tests run SEQUENTIALLY — each spawns
its own backend(s) and some share project-plugin DLL paths, so they must not
overlap. Windows-first; the drivers SKIP themselves on non-nt.

  python tools/run_qa.py              # all qa_* tests
  python tools/run_qa.py group        # only those whose folder name contains "group"
  python tools/run_qa.py --list       # list matching tests, don't run
"""
from __future__ import annotations
import os, subprocess, sys, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
EXAMPLES = REPO / "examples"


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
    tests = discover(filt)
    if opts.get("--list"):
        for t in tests: print(t.parent.name)
        return 0
    if not tests:
        print(f"no qa_* tests match {filt!r}"); return 1

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
        tag = {"PASS": "PASS", "FAIL": "FAIL <<<", "SKIP": "skip",
               "TIMEOUT": "TIMEOUT <<<"}.get(v, v)
        print(f"{tag}   ({dt:.0f}s)")
        if v in ("FAIL", "TIMEOUT") and r is not None:
            for line in (r.stdout or "").splitlines()[-6:]:
                print("      " + line)
        results.append((name, v, dt))
        time.sleep(1.0)   # let the prior test's backend release its port before the next

    npass = sum(1 for _, v, _ in results if v == "PASS")
    nfail = sum(1 for _, v, _ in results if v in ("FAIL", "TIMEOUT"))
    nskip = sum(1 for _, v, _ in results if v == "SKIP")
    print(f"\n=== {npass} passed, {nfail} failed, {nskip} skipped "
          f"({len(results)} total, {sum(d for _, _, d in results):.0f}s) ===")
    if nfail:
        print("FAILED: " + ", ".join(n for n, v, _ in results if v in ("FAIL", "TIMEOUT")))
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
