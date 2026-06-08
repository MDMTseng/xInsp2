"""Pull-model smoke (A-2) — prove the id/pull path end to end through the real
script ABI: a source plugin STAGES a frame via host->emit_resource under res_id
"latest"; a script PULLS it back with xi::use("src").get("latest") +
.image("img") + .data(). Exercises emit_resource -> ResourceStore ->
get_resource / get_resource_image -> xi::Resource proxy.

Flow: open+compile the project (autostart), exchange the source once to stage
"latest", then cmd:run one inspect() and read the vars it set.
"""
from __future__ import annotations
import sys
from pathlib import Path
from harness import (ROOT, REPO_ROOT, BACKEND_EXE, port_open, safe_kill,
                     spawn_backend, wait_log, read_log, PORT_UP_BUDGET)

sys.path.insert(0, str(REPO_ROOT / "tools" / "xinsp2_py"))
from xinsp2 import Client  # noqa: E402

PROJ = ROOT / "pull_proj"
PORT = 7911


def main() -> int:
    if not BACKEND_EXE.exists():
        print("SKIP"); return 0
    if port_open(PORT):
        print(f"FAIL: :{PORT} in use"); return 1
    log = ROOT / "pull_be.log"
    proc = spawn_backend(PORT, log, [f"--project={PROJ}", "--script=inspect.cpp"])
    try:
        if not wait_log(log, "autostart: ready", PORT_UP_BUDGET):
            print("FAIL: not ready"); print(read_log(log)[-800:]); return 1
        with Client(url=f"ws://127.0.0.1:{PORT}/", timeout=30.0) as c:
            c.exchange_instance("src", {})      # stage "latest" (image + {"seq":N})
            rr = c.run()                         # inspect pulls it back

        ok   = rr.value("ok")
        w    = rr.value("w")
        h    = rr.value("h")
        data = rr.value("data") or ""
        print("\n==== PULL SMOKE ====")
        print(f"ok={ok} w={w} h={h} data={data!r}")
        passed = (ok == 1 and w == 4 and h == 3 and '"seq"' in str(data))
        print("VERDICT:", "PASS" if passed else "FAIL")
        if not passed:
            print(read_log(log)[-800:])
        return 0 if passed else 1
    finally:
        safe_kill(proc, "xinsp-backend.exe")


if __name__ == "__main__":
    raise SystemExit(main())
