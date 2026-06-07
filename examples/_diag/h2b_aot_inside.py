"""H2 regression guard — the containment fix must NOT reject a LEGITIMATE
prebuilt DLL that lives INSIDE the project folder (the real AOT-bundle case).

Build the evil DLL, copy it INTO the project folder, then compile_and_load it
by an in-project absolute path. Expect: load SUCCEEDS (no refusal).
"""
from __future__ import annotations
import shutil, sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "h2_evil_proj"
SRC = str(PROJ / "inspect.cpp")
INSIDE = PROJ / "aot_inside.dll"     # lives INSIDE the project
PORT = 7906


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "h2b_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60.0) as c:
            built = Path(c.compile_and_load(SRC, timeout=90)["dll"])
            shutil.copy(built, INSIDE)
            print(f"staged in-project prebuilt -> {INSIDE}")
            try:
                rsp = c.compile_and_load(str(INSIDE), timeout=30)
                print(f"in-project prebuilt load -> OK: {rsp.get('dll')}")
                print("=> H2 regression PASS: in-project AOT DLL still loads")
                return 0
            except Exception as e:
                print(f"in-project prebuilt load REFUSED: {type(e).__name__}: {str(e)[:120]}")
                print("=> H2 regression FAIL: containment wrongly rejects in-project DLL")
                return 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")
        if INSIDE.exists():
            try: INSIDE.unlink()
            except OSError: pass


if __name__ == "__main__":
    raise SystemExit(main())
