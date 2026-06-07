"""R2-#7 PoC — non-finite VAR values no longer corrupt the vars message.

Before: VAR(x, NaN) -> std::to_string -> "nan" -> invalid JSON in the vars frame
-> the client's parse fails and the whole frame is lost (or a finite read returns
0.0). After: emitted as a quoted sentinel "NaN"/"Infinity", so the frame parses
and the value stays visibly non-finite.

Verdict: run returns vars, nan_val/inf_val are the sentinels (not 0.0/missing),
ok_val round-trips as 42.5.
"""
from __future__ import annotations
import sys, time
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "r2_nan_proj"
PORT = 7911


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "r2_nan_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            r = c.run(timeout=15.0)
            nan_v = r.value("nan_val", "<missing>")
            inf_v = r.value("inf_val", "<missing>")
            ok_v = r.value("ok_val", "<missing>")
            print(f"nan_val={nan_v!r}  inf_val={inf_v!r}  ok_val={ok_v!r}")
            print("\n==== R2-#7 VERDICT ====")
            got_frame = (ok_v == 42.5)
            sentinels = (str(nan_v) == "NaN" and "Infinity" in str(inf_v))
            if got_frame and sentinels:
                print("=> FIXED: vars frame parses; non-finite values are visible "
                      "sentinels, not 0.0/lost; finite values round-trip")
            elif not got_frame:
                print("=> vars frame missing/corrupt — NaN may still be breaking JSON")
            else:
                print(f"=> partial: ok_val ok but sentinels off (nan={nan_v!r} inf={inf_v!r})")
        return 0
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
