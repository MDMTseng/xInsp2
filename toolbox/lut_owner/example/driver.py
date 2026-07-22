"""lut_owner example — proof that the handle pattern actually holds up.

Three things get asserted, because the pattern is only worth anything if all
three are true at once:

  BUILD ONCE      many ticks of grading, and the owner's lifetime `builds`
                  counter is still 1. If a handle did not really outlive its
                  tick, this number would climb with the frames.

  LEASE, NOT      `recycle_all` is sent mid-run, which bumps every slot's
  POINTER         generation and strands the handle the script is holding. The
                  script must SEE that (a `stale_handle` $fault, not a crash
                  and not a wrong answer), rebuild, and go back to grading
                  correctly — and the owner's books must show the recycle and
                  the second build.

  OPTIONAL        the same project without the `lut` instance still runs. The
                  consumer reports have_cap=0, the script returns "not
                  applicable" (code 0) instead of a defect, and nothing faults.

Run:  python toolbox/lut_owner/example/driver.py
"""
from __future__ import annotations
import json, os, re, shutil, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[2]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "qa" / "lib"))
from ports import free_port          # noqa: E402
from backends import backend_built, spawn_backend, connect  # noqa: E402

PORT = int(os.environ.get("PORT", "0")) or free_port()
MSG = re.compile(r"seq=(-?\d+) code=(-?\d+) grade=(-?\d+) builds=(-?\d+) stale=(\d)")


def drain(c, seconds: float) -> list[tuple[int, str]]:
    """Collect (code, msg) from run_result events for `seconds`."""
    out: list[tuple[int, str]] = []
    end = time.time() + seconds
    while time.time() < end:
        try:
            ev = c._inbox_events.get(timeout=max(0.05, end - time.time()))
        except Exception:
            break
        if ev.get("name") == "run_result":
            d = ev.get("data", {})
            out.append((d.get("code"), d.get("msg", "")))
    return out


def project_without_owner() -> Path:
    """A throwaway copy of this project with the lut_owner instance removed."""
    dst = Path(tempfile.gettempdir()) / f"xi_lut_ex_noowner_{PORT}"
    shutil.rmtree(dst, ignore_errors=True)
    dst.mkdir(parents=True)
    shutil.copyfile(ROOT / "inspect.cpp", dst / "inspect.cpp")
    shutil.copytree(ROOT / "instances" / "grader", dst / "instances" / "grader")
    shutil.copytree(ROOT / "plugins", dst / "plugins")
    proj = json.loads((ROOT / "project.json").read_text())
    proj["name"] = "lut_owner_example_no_owner"
    proj["instances"] = [i for i in proj["instances"] if i["plugin"] != "lut_owner"]
    (dst / "project.json").write_text(json.dumps(proj, indent=2))
    return dst


def main() -> int:
    if not backend_built():
        print("SKIP: backend not built"); return 0
    fails: list[str] = []
    proc = spawn_backend(PORT, ROOT / f"backend_{PORT}.log", tag="xi_lutowner_ex")
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})
        c.compile_and_load(str(ROOT / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 10})

        # ---- BUILD ONCE, QUERY MANY --------------------------------------
        runs = drain(c, 4.0)
        stats = c.exchange_instance("lut", {"command": "stats"}) or {}
        print(f"steady:   runs={len(runs)} stats={stats}")
        parsed = [MSG.search(m) for _, m in runs]
        oks = [r for r in runs if r[0] == 1]

        if len(oks) < 8:
            fails.append(f"only {len(oks)} graded runs: {runs[:3]}")
        if any(code == -1 for code, _ in runs):
            fails.append(f"defect verdicts while grading: "
                         f"{[m for cd, m in runs if cd == -1][:3]}")
        if any(p is None for p in parsed):
            fails.append("a run produced no parseable grading message")
        if stats.get("builds") != 1:
            fails.append(f"builds={stats.get('builds')} after {len(runs)} runs "
                         "— the handle did not outlive its tick")
        if stats.get("queries", 0) < len(oks):
            fails.append(f"queries={stats.get('queries')} for {len(oks)} graded "
                         "runs — the table was not actually consulted")
        if stats.get("registered") is not True:
            fails.append(f"provider not registered: {stats!r}")

        # ---- A HANDLE IS A LEASE -----------------------------------------
        c.exchange_instance("lut", {"command": "recycle_all"})
        after = drain(c, 4.0)
        s2 = c.exchange_instance("lut", {"command": "stats"}) or {}
        saw_stale = [m for cd, m in after if MSG.search(m or "")
                     and MSG.search(m).group(5) == "1"]
        print(f"recycled: runs={len(after)} stale_reports={len(saw_stale)} stats={s2}")

        if not saw_stale:
            fails.append("no run reported a stale handle after recycle_all — "
                         "the strand was never noticed")
        if s2.get("stale_faults", 0) < 1:
            fails.append(f"owner logged no stale fault: {s2!r}")
        if s2.get("recycles", 0) < 1:
            fails.append(f"owner logged no recycle: {s2!r}")
        if s2.get("builds") != 2:
            fails.append(f"builds={s2.get('builds')} after the recycle, want 2 "
                         "(one rebuild, and only one)")
        tail = [cd for cd, _ in after[-4:]]
        if tail and not all(cd == 1 for cd in tail):
            fails.append(f"grading did not recover after the recycle: {tail}")

        c.call("stop")
        c.call("close_project")

        # ---- THE PROVIDER IS OPTIONAL ------------------------------------
        noowner = project_without_owner()
        c.call("open_project", {"path": str(noowner)})
        c.compile_and_load(str(noowner / "inspect.cpp"), timeout=300)
        c.call("start", {"fps": 10})
        degraded = drain(c, 3.0)
        print(f"no owner: runs={len(degraded)} sample={degraded[:1]}")

        if len(degraded) < 5:
            fails.append(f"only {len(degraded)} runs with no provider — the "
                         "pipeline did not survive the capability being absent")
        elif not all(cd == 0 for cd, _ in degraded):
            fails.append("with no provider the script should report "
                         f"not-applicable (code 0), got {set(cd for cd, _ in degraded)}")

        c.call("stop")
        c.call("close_project"); c.close()
    except Exception as e:
        fails.append(f"{e!r}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
