"""
Script-uses-external-DLL experiment (proves project.json include_dirs + link_libs
and the project-folder DLL search path).

  1. Build extmath.c -> extmath.dll (+ extmath.lib) into the project folder.
  2. open_project + compile_and_load inspect.cpp:
       - <extmath.h> resolves via "include_dirs": ["include"]   (A: include hook)
       - extmath.lib links via "link_libs": ["extmath.lib"]     (A: lib hook)
  3. The script DLL loads only if extmath.dll is found at runtime — it lives in
     the project folder, which the backend put on the DLL search path  (B).
  4. run() once and assert ext_add(2,3) == 5.

A clean compile_and_load already proves A (link) + B (runtime load); the run()
check confirms the call actually executes.

Run:  python examples/script_external_dll/run_experiment.py   (Windows; backend built)
"""
from __future__ import annotations
import os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

BACKEND = REPO / "backend" / "build" / "Release" / "xinsp-backend.exe"
PORT = int(os.environ.get("PORT", "7866"))


def build_extdll(vcvars: str) -> None:
    """Compile extmath.c -> extmath.dll (+ extmath.lib) in the project folder."""
    bat = ROOT / "_buildext.bat"
    bat.write_text(
        "@echo off\r\n"
        f'call "{vcvars}" >nul 2>nul\r\n'
        'cl /nologo /LD extmath.c /Fe:extmath.dll >nul 2>nul\r\n',
        encoding="ascii",
    )
    subprocess.run(["cmd", "/c", str(bat)], cwd=str(ROOT), check=False)
    if not (ROOT / "extmath.dll").exists() or not (ROOT / "extmath.lib").exists():
        raise RuntimeError("failed to build extmath.dll/.lib")


def main() -> int:
    if os.name != "nt":
        print("SKIP: Windows-only"); return 0
    if not BACKEND.exists():
        print(f"SKIP: backend not built ({BACKEND})"); return 0

    log = open(ROOT / "backend.log", "w", encoding="utf-8")
    proc = subprocess.Popen([str(BACKEND), f"--port={PORT}"], stdout=log,
                            stderr=subprocess.STDOUT, cwd=str(REPO))
    failures: list[str] = []
    try:
        c = None
        for _ in range(80):
            try:
                c = Client(url=f"ws://127.0.0.1:{PORT}/", timeout=120); c.connect(); c.ping(); break
            except Exception:
                time.sleep(0.5); c = None
        if not c:
            print("FAIL: backend never came up"); return 1

        c.call("open_project", {"path": str(ROOT)})

        vcvars = next(x["path"] for x in c.call("toolchain_health")["components"]
                      if x["key"] == "vcvars")
        print(f"vcvars: {vcvars}")
        build_extdll(vcvars)
        print("built extmath.dll + extmath.lib in project folder")

        # The real test: this links extmath.lib (A) and the resulting script DLL
        # only loads if extmath.dll is found in the project folder (B).
        rsp = c.call("compile_and_load", {"path": str(ROOT / "inspect.cpp")})
        print("compile_and_load OK (linked extmath.lib + loaded with extmath.dll resolved)")

        r = c.run()
        sval = r.var("sum"); okv = r.var("ok")
        print(f"  sum = {sval and sval.get('value')}   ok = {okv and okv.get('value')}")
        if not sval or sval.get("value") != 5:
            failures.append(f"expected sum=5, got {sval}")

        print("VERDICT:", "PASS" if not failures else "FAIL " + "; ".join(failures))
        return 0 if not failures else 1
    except Exception as e:
        print(f"FAIL: {e}")
        return 1
    finally:
        try: c and c.call("close_project")
        except Exception: pass
        proc.terminate()
        try: proc.wait(5)
        except Exception: proc.kill()


if __name__ == "__main__":
    sys.exit(main())
