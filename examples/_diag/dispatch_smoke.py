"""Phase-B smoke — prove dispatch-by-res_id with NO trigger bus: a source mints
an id, stages a frame under it (emit_resource), and signals a run for it
(emit_dispatch). The script reads the id back via current_trigger().id_string()
and pulls the frame with xi::use("src").get(id). Exercises emit_dispatch ->
dispatch sink -> id-only lane event -> run -> pull.

Non-continuous: exchange the source once; emit_dispatch routes to the one-shot
path; the dispatched run emits a vars event we read back.
"""
from __future__ import annotations
import sys
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "dispatch_proj"
PORT = 7912


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "dispatch_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            c.exchange_instance("src", {})        # stage + emit_dispatch -> run
            vmsg = c.next_vars(timeout=10.0)        # the dispatched run's vars
        if not vmsg:
            print("FAIL: no vars from emit_dispatch run"); print(read_log(log)[-800:]); return 1
        vals = {it["name"]: it.get("value") for it in vmsg.get("items", [])}
        print("\n==== DISPATCH SMOKE ====")
        print("vars:", vals)
        passed = (vals.get("ok") == 1 and vals.get("w") == 5 and vals.get("h") == 2
                  and '"seq"' in str(vals.get("data", "")))
        print("VERDICT:", "PASS" if passed else "FAIL")
        if not passed:
            print(read_log(log)[-800:])
        return 0 if passed else 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
