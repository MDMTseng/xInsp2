"""H2 PoC — arbitrary absolute-path DLL load via compile_and_load.

compile_and_load treats any path ending in `.dll` as a prebuilt AOT script DLL
and (service_main.cpp:2435-2443) LoadLibraryEx's it directly. Only RELATIVE
paths get resolved under the project folder; an ABSOLUTE/UNC path is loaded
as-is with no containment check.

We build an "evil" DLL whose static initializer drops a marker on load, place
its COPY outside any project, then compile_and_load its absolute path. If the
marker reappears, merely loading the DLL executed our code -> H2 CONFIRMED.
"""
from __future__ import annotations
import shutil, sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

EVIL_PROJ = ROOT / "h2_evil_proj"
EVIL_SRC = str(EVIL_PROJ / "inspect.cpp")
EVIL_COPY = ROOT / "evil_copy.dll"          # lives OUTSIDE any project
MARKER = ROOT / "h2_marker.txt"
PORT = 7904


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP: backend exe missing"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} already in use"); return 1
    log = ROOT / "h2_be.log"
    proc = spawn_backend(PORT, log, [f"--project={EVIL_PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: backend never ready"); print(read_log(log)[-1500:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=60.0) as c:
            # 1) compile the evil script to get its real DLL path
            rsp = c.compile_and_load(EVIL_SRC, timeout=90)
            built = Path(rsp["dll"])
            print(f"built evil DLL -> {built}")
            # 2) stage a copy OUTSIDE the project tree
            shutil.copy(built, EVIL_COPY)
            print(f"staged copy -> {EVIL_COPY}")
            # 3) clear the marker (it was written when the build was loaded)
            if MARKER.exists():
                MARKER.unlink()
            print(f"marker cleared: exists={MARKER.exists()}")
            # 4) load the prebuilt DLL by ABSOLUTE path (out-of-tree)
            t0 = time.time()
            refused = False
            refusal = ""
            try:
                rsp2 = c.compile_and_load(str(EVIL_COPY), timeout=30)
                print(f"compile_and_load(abs evil.dll) -> {rsp2} in {time.time()-t0:.1f}s")
            except Exception as e:
                refused = True
                refusal = f"{type(e).__name__}: {str(e)[:110]}"
                print(f"compile_and_load(abs evil.dll) REFUSED: {refusal}")
            time.sleep(0.6)

            print("\n==== H2 VERDICT ====")
            if MARKER.exists():
                print(f"marker RE-CREATED by loading {EVIL_COPY.name}: "
                      f"{MARKER.read_text(errors='ignore').strip()!r}")
                print("=> H2 still vulnerable: arbitrary out-of-tree DLL executed")
            elif refused:
                print(f"out-of-tree DLL load refused + marker NOT recreated")
                print("=> H2 FIXED (prebuilt DLL containment rejects out-of-tree path)")
            else:
                print("marker NOT recreated but load not refused -> investigate")
                print("---- log tail ----")
                print(read_log(log)[-800:])
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")
        if EVIL_COPY.exists():
            try: EVIL_COPY.unlink()
            except OSError: pass


if __name__ == "__main__":
    raise SystemExit(main())
