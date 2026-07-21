"""
get_dashboard — the BE serves the project's HMI dashboard so the HMI only needs
the WS URL (no filesystem coupling). Asserts:
  - get_dashboard {} returns the project's dashboard.json (found + title + layout);
  - get_dashboard {name:"trends"} returns dashboard.trends.json;
  - get_dashboard {name:"missing"} returns found:false;
  - a path-escape name ("../secret") is rejected (found:false, no traversal).

Run:  python examples/qa_get_dashboard/driver.py   (Windows; backend built)
"""
from __future__ import annotations
import os
import tempfile, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
sys.path.insert(0, str(REPO / "examples" / "lib"))
from xinsp2 import Client  # noqa: E402
from ports import free_port, backend_exe  # noqa: E402

BACKEND = backend_exe()
PORT = int(os.environ.get("PORT", "0")) or free_port()


def spawn(port):
    iso = Path(tempfile.gettempdir()) / "xi_gd_iso"
    iso.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ); env["TEMP"] = env["TMP"] = env["TMPDIR"] = str(iso)
    log = open(ROOT / f"backend_{port}.log", "w", encoding="utf-8")
    return subprocess.Popen([str(BACKEND), f"--port={port}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO), env=env)


def connect(port):
    for _ in range(80):
        try:
            c = Client(url=f"ws://127.0.0.1:{port}/", timeout=60); c.connect(); c.ping(); return c
        except Exception:
            time.sleep(0.5)
    return None


def main() -> int:
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0
    fails: list[str] = []
    proc = spawn(PORT)
    try:
        c = connect(PORT)
        assert c, "no connect"
        c.call("open_project", {"path": str(ROOT)})

        d = c.call("get_dashboard", {})
        print(f"default: found={d.get('found')} title={d.get('dashboard', {}).get('title')!r}")
        if not d.get("found"): fails.append("default dashboard not found")
        elif d["dashboard"].get("title") != "Line A — operator":
            fails.append(f"default title wrong: {d['dashboard'].get('title')!r}")
        elif "layout" not in d["dashboard"]:
            fails.append("default dashboard missing layout")

        t = c.call("get_dashboard", {"name": "trends"})
        print(f"trends:  found={t.get('found')} title={t.get('dashboard', {}).get('title')!r}")
        if not (t.get("found") and t["dashboard"].get("title") == "Line A — trends"):
            fails.append("named dashboard 'trends' not served correctly")

        m = c.call("get_dashboard", {"name": "missing"})
        print(f"missing: found={m.get('found')}")
        if m.get("found"): fails.append("missing dashboard reported found")

        e = c.call("get_dashboard", {"name": "../project"})
        print(f"escape:  found={e.get('found')}")
        if e.get("found"): fails.append("path-escape name was served (traversal!)")

        c.call("close_project"); c.close()
    except Exception as ex:
        fails.append(f"{ex}")
    finally:
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()

    print("VERDICT:", "PASS" if not fails else "FAIL: " + "; ".join(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
